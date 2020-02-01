#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <asm/unistd.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/syscalls.h>
#include "interceptor.h"

//AABC
MODULE_DESCRIPTION("My kernel module");
MODULE_AUTHOR("Me");
MODULE_LICENSE("GPL");

//----- System Call Table Stuff ------------------------------------
/* Symbol that allows access to the kernel system call table */
extern void* sys_call_table[];

/* The sys_call_table is read-only => must make it RW before replacing a syscall */
void set_addr_rw(unsigned long addr) {

	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	if (pte->pte &~ _PAGE_RW) pte->pte |= _PAGE_RW;

}

/* Restores the sys_call_table as read-only */
void set_addr_ro(unsigned long addr) {

	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	pte->pte = pte->pte &~_PAGE_RW;

}
//-------------------------------------------------------------


//----- Data structures and bookkeeping -----------------------
/**
 * This block contains the data structures needed for keeping track of
 * intercepted system calls (including their original calls), pid monitoring
 * synchronization on shared data, etc.
 * It's highly unlikely that you will need any globals other than these.
 */

/* List structure - each intercepted syscall may have a list of monitored pids */
struct pid_list {
	pid_t pid;
	struct list_head list;
};


/* Store info about intercepted/replaced system calls */
typedef struct {

	/* Original system call */
	asmlinkage long (*f)(struct pt_regs);

	/* Status: 1=intercepted, 0=not intercepted */
	int intercepted;

	/* Are any PIDs being monitored for this syscall? */
	int monitored;	
	/* List of monitored PIDs */
	int listcount;
	struct list_head my_list;
}mytable;

/* An entry for each system call */
mytable table[NR_syscalls+1];

/* Access to the table and pid lists must be synchronized */
spinlock_t pidlist_lock = SPIN_LOCK_UNLOCKED;
spinlock_t calltable_lock = SPIN_LOCK_UNLOCKED;
//-------------------------------------------------------------


//----------LIST OPERATIONS------------------------------------
/**
 * These operations are meant for manipulating the list of pids 
 * Nothing to do here, but please make sure to read over these functions 
 * to understand their purpose, as you will need to use them!
 */

/**
 * Add a pid to a syscall's list of monitored pids. 
 * Returns -ENOMEM if the operation is unsuccessful.
 */
static int add_pid_sysc(pid_t pid, int sysc){
	struct pid_list *ple=(struct pid_list*)kmalloc(sizeof(struct pid_list), GFP_KERNEL);

	if (!ple)
		return -ENOMEM;

	INIT_LIST_HEAD(&ple->list);
	ple->pid=pid;

	list_add(&ple->list, &(table[sysc].my_list));
	table[sysc].listcount++;

	return 0;
}

/**
 * Remove a pid from a system call's list of monitored pids.
 * Returns -EINVAL if no such pid was found in the list.
 */
static int del_pid_sysc(pid_t pid, int sysc)
{
	struct list_head *i;
	struct pid_list *ple;

	list_for_each(i, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		if(ple->pid == pid) {

			list_del(i);
			kfree(ple);

			table[sysc].listcount--;
			/* If there are no more pids in sysc's list of pids, then
			 * stop the monitoring only if it's not for all pids (monitored=2) */
			if(table[sysc].listcount == 0 && table[sysc].monitored == 1) {
				table[sysc].monitored = 0;
			}

			return 0;
		}
	}

	return -EINVAL;
}

/**
 * Remove a pid from all the lists of monitored pids (for all intercepted syscalls).
 * Returns -1 if this process is not being monitored in any list.
 */
static int del_pid(pid_t pid)
{
	struct list_head *i, *n;
	struct pid_list *ple;
	int ispid = 0, s = 0;

	for(s = 1; s < NR_syscalls; s++) {

		list_for_each_safe(i, n, &(table[s].my_list)) {

			ple=list_entry(i, struct pid_list, list);
			if(ple->pid == pid) {

				list_del(i);
				ispid = 1;
				kfree(ple);

				table[s].listcount--;
				/* If there are no more pids in sysc's list of pids, then
				 * stop the monitoring only if it's not for all pids (monitored=2) */
				if(table[s].listcount == 0 && table[s].monitored == 1) {
					table[s].monitored = 0;
				}
			}
		}
	}

	if (ispid){
		return 0;
	}
	return -1;
}

/**
 * Clear the list of monitored pids for a specific syscall.
 */
static void destroy_list(int sysc) {
 
	struct list_head *i, *n;
	struct pid_list *ple;

	list_for_each_safe(i, n, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		list_del(i);
		kfree(ple);
	}

	table[sysc].listcount = 0;
	table[sysc].monitored = 0;
}

/**
 * Check if two pids have the same owner - useful for checking if a pid 
 * requested to be monitored is owned by the requesting process.
 * Remember that when requesting to start monitoring for a pid, only the 
 * owner of that pid is allowed to request that.
 */
static int check_pid_from_list(pid_t pid1, pid_t pid2) {

	struct task_struct *p1 = pid_task(find_vpid(pid1), PIDTYPE_PID);
	struct task_struct *p2 = pid_task(find_vpid(pid2), PIDTYPE_PID);
	if(p1->real_cred->uid != p2->real_cred->uid){
		return -EPERM;
	}
	return 0;
}

/**
 * Check if a pid is already being monitored for a specific syscall.
 * Returns 1 if it already is, or 0 if pid is not in sysc's list.
 */
static int check_pid_monitored(int sysc, pid_t pid) {

	struct list_head *i;
	struct pid_list *ple;

	list_for_each(i, &(table[sysc].my_list)) {

		ple=list_entry(i, struct pid_list, list);
		if(ple->pid == pid){ 
			return 1;
		}
		
	}
	return 0;	
}
//----------------------------------------------------------------

//----- Intercepting exit_group ----------------------------------
/**
 * Since a process can exit without its owner specifically requesting
 * to stop monitoring it, we must intercept the exit_group system call
 * so that we can remove the exiting process's pid from *all* syscall lists.
 */  

/** 
 * Stores original exit_group function - after all, we must restore it 
 * when our kernel module exits.
 */
void (*orig_exit_group)(int);

/**
 * Our custom exit_group system call.
 *
 * TODO: When a process exits, make sure to remove that pid from all lists.
 * The exiting process's PID can be retrieved using the current variable (current->pid).
 * Don't forget to call the original exit_group.
 */
void my_exit_group(int status)
{
	//dels the exiting process's pid from all syscalls, then calls original exit group
	del_pid(current->pid);
	(*orig_exit_group)(status);
	
}
//----------------------------------------------------------------



/** 
 * This is the generic interceptor function.
 * It should just log a message and call the original syscall.
 * 
 * TODO: Implement this function. 
 * - Check first to see if the syscall is being monitored for the current->pid. 
 * - Recall the convention for the "monitored" flag in the mytable struct: 
 *     monitored=0 => not monitored
 *     monitored=1 => some pids are monitored, check the corresponding my_list
 *     monitored=2 => all pids are monitored for this syscall
 * - Use the log_message macro, to log the system call parameters!
 *     Remember that the parameters are passed in the pt_regs registers.
 *     The syscall parameters are found (in order) in the 
 *     ax, bx, cx, dx, si, di, and bp registers (see the pt_regs struct).
 * - Don't forget to call the original system call, so we allow processes to proceed as normal.
 */
 //

asmlinkage long interceptor(struct pt_regs reg) {
	//FIX THE LOCKS!!
	spin_lock(&pidlist_lock);
	//first we want to check that the system call is being monitored for the current pid
	//if the current pid is being monitored in the systemcall then we log its parameters
	//reg.ax is the first parameter (head of the system call), so we want to check for the pid value there
	if (check_pid_monitored(reg.ax, current->pid) == 1){
		//want to log the parameters of the syscall which are ax,bx,...,bp registers for the current pid
		log_message(current->pid, reg.ax, reg.bx, reg.cx, reg.dx, reg.si, reg.di, reg.bp);
	}
	spin_unlock(&pidlist_lock);
	//we need to call the original system call so processes proceed as normal
	return (table[reg.ax].f(reg));
}

/**
 * My system call - this function is called whenever a user issues a MY_CUSTOM_SYSCALL system call.
 * When that happens, the parameters for this system call indicate one of 4 actions/commands:
 *      - REQUEST_SYSCALL_INTERCEPT to intercept the 'syscall' argument
 *      - REQUEST_SYSCALL_RELEASE to de-intercept the 'syscall' argument
 *      - REQUEST_START_MONITORING to start monitoring for 'pid' whenever it issues 'syscall' 
 *      - REQUEST_STOP_MONITORING to stop monitoring for 'pid'
 *      For the last two, if pid=0, that translates to "all pids".
 * 
 * TODO: Implement this function, to handle all 4 commands correctly.
 *
 * - For each of the commands, check that the arguments are valid (-EINVAL):
 *   a) the syscall must be valid (not negative, not > NR_syscalls, and not MY_CUSTOM_SYSCALL itself)
 *   b) the pid must be valid for the last two commands. It cannot be a negative integer, 
 *      and it must be an existing pid (except for the case when it's 0, indicating that we want 
 *      to start/stop monitoring for "all pids"). 
 *      If a pid belongs to a valid process, then the following expression is non-NULL:
 *           pid_task(find_vpid(pid), PIDTYPE_PID)
 * - Check that the caller has the right permissions (-EPERM)
 *      For the first two commands, we must be root (see the current_uid() macro).
 *      For the last two commands, the following logic applies:
 *        - is the calling process root? if so, all is good, no doubts about permissions.
 *        - if not, then check if the 'pid' requested is owned by the calling process 
 *        - also, if 'pid' is 0 and the calling process is not root, then access is denied 
 *          (monitoring all pids is allowed only for root, obviously).
 *      To determine if two pids have the same owner, use the helper function provided above in this file.
 * - Check for correct context of commands (-EINVAL):
 *     a) Cannot de-intercept a system call that has not been intercepted yet.
 *     b) Cannot stop monitoring for a pid that is not being monitored, or if the 
 *        system call has not been intercepted yet.
 * - Check for -EBUSY conditions:
 *     a) If intercepting a system call that is already intercepted.
 *     b) If monitoring a pid that is already being monitored.
 * - If a pid cannot be added to a monitored list, due to no memory being available,
 *   an -ENOMEM error code should be returned.
 *
 * - Make sure to keep track of all the metadata on what is being intercepted and monitored.
 *   Use the helper functions provided above for dealing with list operations.
 *
 * - Whenever altering the sys_call_table, make sure to use the set_addr_rw/set_addr_ro functions
 *   to make the system call table writable, then set it back to read-only. 
 *   For example: set_addr_rw((unsigned long)sys_call_table);
 *   Also, make sure to save the original system call (you'll need it for 'interceptor' to work correctly).
 * 
 * - Make sure to use synchronization to ensure consistency of shared data structures.
 *   Use the calltable_spinlock and pidlist_spinlock to ensure mutual exclusion for accesses 
 *   to the system call table and the lists of monitored pids. Be careful to unlock any spinlocks 
 *   you might be holding, before you exit the function (including error cases!).  
 */
asmlinkage long my_syscall(int cmd, int syscall, int pid) {
	if (0 <= syscall < NR_syscalls + 1 && syscall != MY_CUSTOM_SYSCALL){
		// if (cmd = REQUEST_SYSCALL_INTERCEPT){
		// 	//check if the user isn't root
		// 	if (current_uid() != 0){
		// 		//raise -EPERM for none root users!
		// 		return -EPERM;
		// 	}
		// 	//can't intercept an already intercepted syscall
		// 	//return -Ebusy error for already intercepted syscall
		// 	if(calltable_lock == TRUE){
		// 		return -EBUSY;
		// 	}
		// 	//use synchronization to moderate syscall commands
		// 	aquire(calltable_lock);
		// 	//we can't stop monitoring a pid until we intercept that pid's system call
		// 	//release the pid lock only after we've intercepted
		// 	release(pidlist_lock);
		// 	//allow for us to make changes to the table
		// 	set_addr_rw(unsigned long sys_call_table);
		// 	//indicating the syscall has been intercepted
		// 	table[syscall].intercepted = 1;
		// 	//using our table to intercept the syscall, changing table to writable
		// 	table[syscall] == sys_call_table[syscall];
		// 	sys_call_table[syscall] = interceptor;
		// 	//restoring syscall table to read only
		// 	set_addr_ro(unsigned long sys_call_table);
		// 	release(calltable_lock);

		// }


		// if (cmd = REQUEST_SYSCALL_RELEASE){
		// 	//cannot de-intercept a command which hasn't been intercepted yet
		// 	//return -EINVAL error because we are trying to de-intercept before intercepting
		// 	if (calltable_lock == TRUE){
		// 		return -EINVAL;
		// 	}
		// 	//de-intercept the intercepted code
		// 	//use synchronization to moderate syscall commands
		// 	aquire(calltable_lock);
		// 	//lock the pidlist whenever the syscall isn't intercepted
		// 	aquire(pidlist_lock);
		// 	//allow for us to make changes to the table
		// 	set_addr_rw(unsigned long sys_call_table);
		// 	//deintercept indicated by 0
		// 	table[syscall].intercepted = 0;
		// 	//using our table to intercept the syscall, changing table to writable
		// 	table[syscall] == sys_call_table[syscall];
		// 	sys_call_table[syscall] = interceptor;
		// 	//restoring syscall table to read only
		// 	set_addr_ro(unsigned long sys_call_table);
		// 	release(calltable_lock);
		// }





		// if (cmd = REQUEST_START_MONITORING){
		// 	//check if the calling process is root or if the pid is owned by the calling process
		// 	//or if pid=0 user must be root
		// 	//raise EPERM error for denied permissions
		// 	if ((current_uid() != 0)  || check_pid_from_list(current->pid, pid)) {
		// 		return -EPERM;
		// 	}
		// 	//pid value can't be negative
		// 	//if its not negative it must be 0 or greater making it valid assuming it exist
		// 	//if it doesn't exist check if pid is 0
		// 	//Raise EINVAL error for invalid parameters
		// 	if ((pid < 0 && pid_task(find_vpid(pid), PIDTYPE_PID) == NULL) || (pid != 0)){
		// 		return -EINVAL;
		// 	}
		// 	//can't monitor a pid that's already being monitored
		// 	//return -ebusy error for pid already being monitored
		// 	if(check_pid_monitored(syscall, pid) == 1){
		// 		return -EBUSY;
		// 	}
		// }
		if (cmd = REQUEST_STOP_MONITORING){
			//if the current user isn't root or the pid isn't owned by the syscall
			//raise EPERM error for denied permissions
			// if ((current_uid() != 0) || check_pid_from_list(current->pid, pid)) {
			// 	return -EPERM;
			// }
			// //pid value can't be negative
			// //if its not negative it must be 0 or greater making it valid assuming it exist
			// //if it doesn't exist check if pid is 0
			// //Raise EINVAL error for invalid parameters
			// if ((pid < 0 && pid_task(find_vpid(pid), PIDTYPE_PID) == NULL) || (pid != 0)) {
			// 	return -EINVAL;
			// }
			// //if the lock is on then the syscall has been intercepted 
			// //also check if pid is already being monitored
			// //we can't stop monitoring a pid that isn't being monitor or pids of a syscall that isn't intercepted
			// //return -EINVAL error because we are trying to de-intercept a command which hasn't been intercepted yet
			// if ((calltable_lock == True ) || check_pid_from_list(current->pid, pid)) {
			// 	return -EBUSY;
			// }
			if (current_uid() == 0) {
				;
			}
			// If curr_id != 0, we need to check if the pid requested is called by the owning process
			else {
				// Case if pid = 0 and we aren't root OR if pid is not owned 
				if (pid == 0 )
					return -EPERM
				// If this func call throws an -EPERM, this whole thing doesn't run right?
				int caller_owns_pid = check_pid_from_list(current->pid, pid)
				if (caller_owns_pid == 0){
					;
				}
				else {
					return -EPERM
				}
			}

			//Checks for context - PART C only needed in request stop monitoring
			// Checks for pid already being monitored - PART D
			if (check_pid_monitored(syscall, pid) == 1) {
				return -EBUSY;
			}

			// NEED TO UNLOCK ACCESS TO MYTABLE
			
			// check if PID is already monitored
			// WTF are we supposed to return??
			if (check_pid_monitored(syscall, pid)==1) {
				// return something return ;
				return 0;
			}
			else {
			// If it isn't already being monitored, add to pid list for specific syscall
				add_pid_sysc(pid, syscall);
				// return something ;
				return 0;
			}

			// NEED TO LOCK ACCESS TO MYTABLE
		}
	}
}

/**
 *
 */
long (*orig_custom_syscall)(void);


/**
 * Module initialization. 
 *
 * TODO: Make sure to:  
 * - Hijack MY_CUSTOM_SYSCALL and save the original in orig_custom_syscall.
 * - Hijack the exit_group system call (__NR_exit_group) and save the original 
 *   in orig_exit_group.
 * - Make sure to set the system call table to writable when making changes, 
 *   then set it back to read only once done.
 * - Perform any necessary initializations for bookkeeping data structures. 
 *   To initialize a list, use 
 *        INIT_LIST_HEAD (&some_list);
 *   where some_list is a "struct list_head". 
 * - Ensure synchronization as needed.
 */
static int init_function(void) {
	//MISSING SPIN LOCKS!!
	//save the original values of MY_CUSTOM_SYSCALL and __NR_exit_group (aka Hijack MY CUSTOM SYSCALL)
	orig_custom_syscall = sys_call_table[MY_CUSTOM_SYSCALL];
	orig_exit_group = sys_call_table[__NR_exit_group];
	spin_lock(&calltable_lock);
	//making system call table writable
	set_addr_rw((unsigned long) sys_call_table);
	//initialize the list 
	int count = 0;
	// default system values 
	int defaultValue = 0;
	//initialized list
	struct pid_list some_list
	INIT_LIST_HEAD (&some_list); 
	for(count=0; count < NR_syscalls + 1; count++){
		//initializing syscall values
		table[count].intercepted = defaultValue;
		table[count].monitored = defaultValue;
		table[count].listcount = defaultValue;
				
	}
	//setting system call table back to read only
	set_addr_ro((unsigned long) sys_call_table);
	spin_unlock(&calltable_lock);
	return 0;
}

/**
 * Module exits. 
 *
 * TODO: Make sure to:  
 * - Restore MY_CUSTOM_SYSCALL to the original syscall.
 * - Restore __NR_exit_group to its original syscall.
 * - Make sure to set the system call table to writable when making changes, 
 *   then set it back to read only once done.
 * - Ensure synchronization, if needed.
 */
static void exit_function(void)
{

	//implement locks!!!
	spin_lock(&calltable_lock);
	//need to loop through each syscall and de intercept any intercepted calls
	int count = 0;
	for(count = 0 ; count<NR_syscalls+1; count++){
		//call mysyscall to release the intercepted systemcall
		if(table[count].intercepted == 1){
			my_syscall(REQUEST_SYSCALL_RELEASE, count, current->pid);
		}
	}
	//making system call table writable
	set_addr_rw((unsigned long) sys_call_table);
	//restoring the original values of the syscall table that were stored in
	//our origcustomsyscall and origexitgroup.
	sys_call_table[MY_CUSTOM_SYSCALL] = orig_custom_syscall;
	sys_call_table[__NR_exit_group] = orig_exit_group;
	//setting system call table back to read only
	set_addr_ro((unsigned long) sys_call_table);
	spin_unlock(&calltable_lock);

}

module_init(init_function);
module_exit(exit_function);

