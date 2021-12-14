// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * livepatch.c - powerpc-specific Kernel Live Patching Core
 *
 * Copyright (C) 2018  Huawei Technologies Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/livepatch.h>
#include <linux/sched/debug.h>
#include <asm/livepatch.h>
#include <asm/cacheflush.h>
#include <linux/slab.h>
#include <asm/code-patching.h>

#if defined (CONFIG_LIVEPATCH_STOP_MACHINE_CONSISTENCY) || \
    defined (CONFIG_LIVEPATCH_WO_FTRACE)
#define LJMP_INSN_SIZE 4
#define MAX_SIZE_TO_CHECK (LJMP_INSN_SIZE * sizeof(u32))
#define CHECK_JUMP_RANGE LJMP_INSN_SIZE

struct klp_func_node {
	struct list_head node;
	struct list_head func_stack;
	void *old_func;
	u32 old_insns[LJMP_INSN_SIZE];
};

static LIST_HEAD(klp_func_list);

static struct klp_func_node *klp_find_func_node(void *old_func)
{
	struct klp_func_node *func_node;

	list_for_each_entry(func_node, &klp_func_list, node) {
		if (func_node->old_func == old_func)
			return func_node;
	}

	return NULL;
}
#endif

#ifdef CONFIG_LIVEPATCH_STOP_MACHINE_CONSISTENCY
/*
 * The instruction set on ppc32 is RISC.
 * The instructions of BL and BLA are 010010xxxxxxxxxxxxxxxxxxxxxxxxx1.
 * The instructions of BCL and BCLA are 010000xxxxxxxxxxxxxxxxxxxxxxxxx1.
 * The instruction of BCCTRL is 010011xxxxxxxxxx0000010000100001.
 * The instruction of BCLRL is 010011xxxxxxxxxx0000000000100001.
 */
static bool is_jump_insn(u32 insn)
{
	u32 tmp1 = (insn & 0xfc000001);
	u32 tmp2 = (insn & 0xfc00ffff);

	if ((tmp1 == 0x48000001) || (tmp1 == 0x40000001) ||
	    (tmp2 == 0x4c000421) || (tmp2 == 0x4c000021))
		return true;
	return false;
}

struct klp_func_list {
	struct klp_func_list *next;
	unsigned long func_addr;
	unsigned long func_size;
	const char *func_name;
	int force;
};

struct stackframe {
	unsigned long sp;
	unsigned long pc;
};

struct walk_stackframe_args {
	int enable;
	struct klp_func_list *check_funcs;
	int ret;
};

static inline unsigned long klp_size_to_check(unsigned long func_size,
		int force)
{
	unsigned long size = func_size;

	if (force == KLP_STACK_OPTIMIZE && size > MAX_SIZE_TO_CHECK)
		size = MAX_SIZE_TO_CHECK;
	return size;
}

static inline int klp_compare_address(unsigned long pc, unsigned long func_addr,
		const char *func_name, unsigned long check_size)
{
	if (pc >= func_addr && pc < func_addr + check_size) {
		pr_err("func %s is in use!\n", func_name);
		return -EBUSY;
	}
	return 0;
}

static bool check_jump_insn(unsigned long func_addr)
{
	unsigned long i;
	u32 *insn = (u32*)func_addr;

	for (i = 0; i < CHECK_JUMP_RANGE; i++) {
		if (is_jump_insn(*insn)) {
			return true;
		}
		insn++;
	}
	return false;
}

static int add_func_to_list(struct klp_func_list **funcs, struct klp_func_list **func,
		unsigned long func_addr, unsigned long func_size, const char *func_name,
		int force)
{
	if (*func == NULL) {
		*funcs = (struct klp_func_list*)kzalloc(sizeof(**funcs), GFP_ATOMIC);
		if (!(*funcs))
			return -ENOMEM;
		*func = *funcs;
	} else {
		(*func)->next = (struct klp_func_list*)kzalloc(sizeof(**funcs),
				GFP_ATOMIC);
		if (!(*func)->next)
			return -ENOMEM;
		*func = (*func)->next;
	}
	(*func)->func_addr = func_addr;
	(*func)->func_size = func_size;
	(*func)->func_name = func_name;
	(*func)->force = force;
	(*func)->next = NULL;
	return 0;
}

static int klp_check_activeness_func(struct klp_patch *patch, int enable,
		struct klp_func_list **check_funcs)
{
	int ret;
	struct klp_object *obj;
	struct klp_func *func;
	unsigned long func_addr, func_size;
	struct klp_func_node *func_node;
	struct klp_func_list *pcheck = NULL;

	for (obj = patch->objs; obj->funcs; obj++) {
		for (func = obj->funcs; func->old_name; func++) {
			if (enable) {
				if (func->force == KLP_ENFORCEMENT)
					continue;
				/*
				 * When enable, checking the currently
				 * active functions.
				 */
				func_node = klp_find_func_node(func->old_func);
				if (!func_node ||
				    list_empty(&func_node->func_stack)) {
					/*
					 * No patched on this function
					 * [ the origin one ]
					 */
					func_addr = (unsigned long)func->old_func;
					func_size = func->old_size;
				} else {
					/*
					 * Previously patched function
					 * [ the active one ]
					 */
					struct klp_func *prev;

					prev = list_first_or_null_rcu(
						&func_node->func_stack,
						struct klp_func, stack_node);
					func_addr = (unsigned long)prev->new_func;
					func_size = prev->new_size;
				}
				/*
				 * When preemtion is disabled and the
				 * replacement area does not contain a jump
				 * instruction, the migration thread is
				 * scheduled to run stop machine only after the
				 * excution of instructions to be replaced is
				 * complete.
				 */
				if (IS_ENABLED(CONFIG_PREEMPTION) ||
				    (func->force == KLP_NORMAL_FORCE) ||
				    check_jump_insn(func_addr)) {
					ret = add_func_to_list(check_funcs, &pcheck,
							func_addr, func_size,
							func->old_name, func->force);
					if (ret)
						return ret;
				}
			} else {
				/*
				 * When disable, check for the previously
				 * patched function and the function itself
				 * which to be unpatched.
				 */
				func_node = klp_find_func_node(func->old_func);
				if (!func_node)
					return -EINVAL;
#ifdef CONFIG_PREEMPTION
				/*
				 * No scheduling point in the replacement
				 * instructions. Therefore, when preemption is
				 * not enabled, atomic execution is performed
				 * and these instructions will not appear on
				 * the stack.
				 */
				if (list_is_singular(&func_node->func_stack)) {
					func_addr = (unsigned long)func->old_func;
					func_size = func->old_size;
				} else {
					struct klp_func *prev;

					prev = list_first_or_null_rcu(
						&func_node->func_stack,
						struct klp_func, stack_node);
					func_addr = (unsigned long)prev->new_func;
					func_size = prev->new_size;
				}
				ret = add_func_to_list(check_funcs, &pcheck, func_addr,
						func_size, func->old_name, 0);
				if (ret)
					return ret;
#endif
				func_addr = (unsigned long)func->new_func;
				func_size = func->new_size;
				ret = add_func_to_list(check_funcs, &pcheck, func_addr,
						func_size, func->old_name, 0);
				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static int unwind_frame(struct task_struct *tsk, struct stackframe *frame)
{

	unsigned long *stack;

	if (!validate_sp(frame->sp, tsk, STACK_FRAME_OVERHEAD))
		return -1;

	stack = (unsigned long *)frame->sp;
	frame->sp = stack[0];
	frame->pc = stack[STACK_FRAME_LR_SAVE];
	return 0;
}

void notrace klp_walk_stackframe(struct stackframe *frame,
		int (*fn)(struct stackframe *, void *),
		struct task_struct *tsk, void *data)
{
	while (1) {
		int ret;

		if (fn(frame, data))
			break;
		ret = unwind_frame(tsk, frame);
		if (ret < 0)
			break;
	}
}

static bool check_func_list(struct klp_func_list *funcs, int *ret, unsigned long pc)
{
	while (funcs != NULL) {
		*ret = klp_compare_address(pc, funcs->func_addr, funcs->func_name,
					klp_size_to_check(funcs->func_size, funcs->force));
		if (*ret) {
			return false;
		}
		funcs = funcs->next;
	}
	return true;
}

static int klp_check_jump_func(struct stackframe *frame, void *data)
{
	struct walk_stackframe_args *args = data;
	struct klp_func_list *check_funcs = args->check_funcs;

	if (!check_func_list(check_funcs, &args->ret, frame->pc)) {
		return args->ret;
	}
	return 0;
}

static void free_list(struct klp_func_list **funcs)
{
	struct klp_func_list *p;

	while (*funcs != NULL) {
		p = *funcs;
		*funcs = (*funcs)->next;
		kfree(p);
	}
}

int klp_check_calltrace(struct klp_patch *patch, int enable)
{
	struct task_struct *g, *t;
	struct stackframe frame;
	unsigned long *stack;
	int ret = 0;
	struct klp_func_list *check_funcs = NULL;
	struct walk_stackframe_args args = {
		.ret = 0
	};

	ret = klp_check_activeness_func(patch, enable, &check_funcs);
	if (ret)
		goto out;
	args.check_funcs = check_funcs;

	for_each_process_thread(g, t) {
		if (t == current) {
			/*
			 * Handle the current carefully on each CPUs, we shouldn't
			 * use saved FP and PC when backtrace current. It's difficult
			 * to backtrack other CPU currents here. But fortunately,
			 * all CPUs will stay in this function, so the current's
			 * backtrace is so similar
			 */
			stack = (unsigned long *)current_stack_pointer;
		} else if (strncmp(t->comm, "migration/", 10) == 0) {
			/*
			 * current on other CPU
			 * we call this in stop_machine, so the current
			 * of each CPUs is mirgation, just compare the
			 * task_comm here, because we can't get the
			 * cpu_curr(task_cpu(t))). This assumes that no
			 * other thread will pretend to be a stopper via
			 * task_comm.
			 */
			continue;
		} else {
			/*
			 * Skip the first frame since it does not contain lr
			 * at normal position and nip is stored in the lr
			 * position in the second frame.
			 * See arch/powerpc/kernel/entry_32.S _switch .
			 */
			unsigned long s = *(unsigned long *)t->thread.ksp;

			if (!validate_sp(s, t, STACK_FRAME_OVERHEAD))
				continue;
			stack = (unsigned long *)s;
		}

		frame.sp = (unsigned long)stack;
		frame.pc = stack[STACK_FRAME_LR_SAVE];
		if (check_funcs != NULL) {
			klp_walk_stackframe(&frame, klp_check_jump_func, t, &args);
			if (args.ret) {
				ret = args.ret;
				pr_info("PID: %d Comm: %.20s\n", t->pid, t->comm);
				show_stack(t, NULL, KERN_INFO);
				goto out;
			}
		}
	}

out:
	free_list(&check_funcs);
	return ret;
}
#endif

#ifdef CONFIG_LIVEPATCH_WO_FTRACE
static inline bool offset_in_range(unsigned long pc, unsigned long addr,
				   long range)
{
	long offset = addr - pc;

	return (offset >= -range && offset < range);
}

int arch_klp_patch_func(struct klp_func *func)
{
	struct klp_func_node *func_node;
	unsigned long pc, new_addr;
	long ret;
	int memory_flag = 0;
	int i;
	u32 insns[LJMP_INSN_SIZE];

	func_node = klp_find_func_node(func->old_func);
	if (!func_node) {
		func_node = func->func_node;
		if (!func_node)
			return -ENOMEM;

		memory_flag = 1;
		INIT_LIST_HEAD(&func_node->func_stack);
		func_node->old_func = func->old_func;
		for (i = 0; i < LJMP_INSN_SIZE; i++) {
			ret = copy_from_kernel_nofault(&func_node->old_insns[i],
				((u32 *)func->old_func) + i, LJMP_INSN_SIZE);
			if (ret) {
				return -EPERM;
			}
		}

		list_add_rcu(&func_node->node, &klp_func_list);
	}

	list_add_rcu(&func->stack_node, &func_node->func_stack);

	pc = (unsigned long)func->old_func;
	new_addr = (unsigned long)func->new_func;
	if (offset_in_range(pc, new_addr, SZ_32M)) {
		struct ppc_inst instr;

		create_branch(&instr, (struct ppc_inst *)pc, new_addr, 0);
		if (patch_instruction((struct ppc_inst *)pc, instr))
			goto ERR_OUT;
	} else {
		/*
		 * lis r12,sym@ha
		 * addi r12,r12,sym@l
		 * mtctr r12
		 * bctr
		 */
		insns[0] = 0x3d800000 + ((new_addr + 0x8000) >> 16);
		insns[1] = 0x398c0000 + (new_addr & 0xffff);
		insns[2] = 0x7d8903a6;
		insns[3] = 0x4e800420;

		for (i = 0; i < LJMP_INSN_SIZE; i++) {
			ret = patch_instruction((struct ppc_inst *)(((u32 *)pc) + i),
						ppc_inst(insns[i]));
			if (ret)
				goto ERR_OUT;
		}
	}

	return 0;

ERR_OUT:
	list_del_rcu(&func->stack_node);
	if (memory_flag) {
		list_del_rcu(&func_node->node);
	}

	return -EPERM;
}

void arch_klp_unpatch_func(struct klp_func *func)
{
	struct klp_func_node *func_node;
	struct klp_func *next_func;
	unsigned long pc, new_addr;
	u32 insns[LJMP_INSN_SIZE];
	int i;

	func_node = klp_find_func_node(func->old_func);
	pc = (unsigned long)func_node->old_func;
	if (list_is_singular(&func_node->func_stack)) {
		for (i = 0; i < LJMP_INSN_SIZE; i++)
			insns[i] = func_node->old_insns[i];

		list_del_rcu(&func->stack_node);
		list_del_rcu(&func_node->node);

		for (i = 0; i < LJMP_INSN_SIZE; i++)
			patch_instruction((struct ppc_inst *)(((u32 *)pc) + i),
					  ppc_inst(insns[i]));
	} else {
		list_del_rcu(&func->stack_node);
		next_func = list_first_or_null_rcu(&func_node->func_stack,
					struct klp_func, stack_node);

		new_addr = (unsigned long)next_func->new_func;
		if (offset_in_range(pc, new_addr, SZ_32M)) {
			struct ppc_inst instr;

			create_branch(&instr, (struct ppc_inst *)pc, new_addr, 0);
			patch_instruction((struct ppc_inst *)pc, instr);
		} else {
			/*
			 * lis r12,sym@ha
			 * addi r12,r12,sym@l
			 * mtctr r12
			 * bctr
			 */
			insns[0] = 0x3d800000 + ((new_addr + 0x8000) >> 16);
			insns[1] = 0x398c0000 + (new_addr & 0xffff);
			insns[2] = 0x7d8903a6;
			insns[3] = 0x4e800420;

			for (i = 0; i < LJMP_INSN_SIZE; i++)
				patch_instruction((struct ppc_inst *)(((u32 *)pc) + i),
						  ppc_inst(insns[i]));
		}
	}
}

/* return 0 if the func can be patched */
int arch_klp_func_can_patch(struct klp_func *func)
{
	unsigned long pc = (unsigned long)func->old_func;
	unsigned long new_addr = (unsigned long)func->new_func;
	unsigned long old_size = func->old_size;

	if (!old_size)
		return -EINVAL;

	if (!offset_in_range(pc, new_addr, SZ_32M) &&
	    (old_size < LJMP_INSN_SIZE * sizeof(u32))) {
		pr_err("func %s size less than limit\n", func->old_name);
		return -EPERM;
	}
	return 0;
}

void arch_klp_mem_prepare(struct klp_patch *patch)
{
	struct klp_object *obj;
	struct klp_func *func;

	klp_for_each_object(patch, obj) {
		klp_for_each_func(obj, func) {
			func->func_node = kzalloc(sizeof(struct klp_func_node),
					GFP_ATOMIC);
		}
	}
}

void arch_klp_mem_recycle(struct klp_patch *patch)
{
	struct klp_object *obj;
	struct klp_func *func;
	struct klp_func_node *func_node;

	klp_for_each_object(patch, obj) {
		klp_for_each_func(obj, func) {
			func_node = func->func_node;
			if (func_node && list_is_singular(&func_node->func_stack)) {
				kfree(func_node);
				func->func_node = NULL;
			}
		}
	}
}
#endif
