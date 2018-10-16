#include <video.h>
#include <types.h>
#include <heapmngr.h>
#include <kutils.h>
#include <proc.h>
#include <sched.h>
#include <vesa.h>
#include <elf.h>
#include <fat.h>
#include <timer.h>
int IdleTask(void);
volatile task_t *current_task;
volatile task_t *ready_queue;
 
tree_t * process_tree;  /* Parent->Children tree */
list_t * process_list;  /* Flat storage */
list_t * process_queue; /* Ready queue */
list_t * sleep_queue;
volatile bool task_switching = true;
extern uintptr_t vesa_com_start;
extern uintptr_t vesa_com_end;
const u16 TIMER_HZ = 100;
void exit();
u32 counter = 0;
extern struct page_directory *current_directory;
u32 volatile pid = 0;
 
extern tree_t * process_tree;
#define KERNEL_STACK_SIZE  4096
void _task_initialize(void)
{
	 __asm__ __volatile__("cli");
	current_task = ready_queue = (task_t*)valloc(sizeof(task_t));

	memset((task_t *)current_task, 0, sizeof(task_t));
	current_task->id = pid++;
        current_task->esp = 0;
        current_task->eip = 0;
        current_task->privilege = 0;
        current_task->state = TASK_RUNNING;
        current_task->next = 0;
        current_task->type = THREAD;
        current_task->priority = PRIO_HIGH;
        current_task->time_to_run = 10;
        current_task->ready_to_run = 1;
	current_task->wd_name = strdup("/");
	current_task->kernel_stack = (u32)valloc(KERNEL_STACK_SIZE)+KERNEL_STACK_SIZE;
 

	/* Allocate space for a new process */
	 
	/* Set it as the root process */
	tree_set_root(process_tree, (void *)current_task);
	/* Set its tree entry pointer so we can keep track
	 * of the process' entry in the process tree. */
	current_task->tree_entry = process_tree->root;
	current_task->id      = 1;       /* Init is PID 1 */
	current_task->group   = 0;
	current_task->name    = strdup("init");  /* Um, duh. */
	current_task->cmdline = NULL;
	current_task->user    = 0;       /* UID 0 */
	current_task->mask    = 022;     /* umask */
	current_task->group   = 0;       /* Task group 0 */
	//current_task->status  = 0;       /* Run status */
	current_task->fds = malloc(sizeof(fd_table_t));
	current_task->fds->refs = 1;
	current_task->fds->length   = 12;  /* Initialize the file descriptors */
	current_task->fds->capacity = 44;
	current_task->fds->entries  = malloc(sizeof(fs_node_t *) * current_task->fds->capacity);
	current_task->fds->entries[0] =  clone_fs(fs_root);
	/* Set the working directory */
	 current_task->wd_node = clone_fs(fs_root);
	current_task->wd_name = strdup("/");

	/* Heap and stack pointers (and actuals) */
	current_task->image.entry       = 0;
	current_task->image.heap        = 0;
	current_task->image.heap_actual = 0;
	//current_task->image.stack       = initial_esp + 1;
	current_task->image.user_stack  = 0;
	current_task->image.size        = 0;
	current_task->image.shm_heap    = SHM_START; /* Yeah, a bit of a hack. */

	//spin_init(current_task->image.lock);

	/* Process is not finished */
	current_task->finished = 0;
	current_task->started = 1;
	current_task->running = 1;
	current_task->wait_queue = list_create();
	current_task->shm_mappings = list_create();
	current_task->signal_queue = list_create();
	current_task->signal_kstack = NULL; /* None yet initialized */

	current_task->sched_node.prev = NULL;
	current_task->sched_node.next = NULL;
	//current_task->sched_node.value = init;

	current_task->sleep_node.prev = NULL;
	current_task->sleep_node.next = NULL;
	//current_task->sleep_node.value = init;

	current_task->timed_sleep_node = NULL;

	current_task->is_tasklet = 0;
 	tree_node_create(current_task);
	//
 
	scheduler_install();
	//insert_current_task(current_task);
	list_insert(process_list, (void *)current_task);
	 __asm__ __volatile__("sti");
}

u32 geteip()
{
	return current_task->eip;
}

s32 getpid()
{
	return current_task->id;
}
#define USER_STACK_START 0xbffff000
void _get_task_stack(task_t *new_task,void (*entry)(),size_t argc, char** argv,u8 privilege, int priority,task_type type)
{
	
	__asm__ __volatile__("cli");
	
	task_switching = false;
	//memset(new_task, 0, sizeof(task_t));
	new_task->kernel_stack = (u32)valloc(KERNEL_STACK_SIZE)+KERNEL_STACK_SIZE;
	
    
	new_task->state = TASK_RUNNING;
	
		current_task->priority = priority;
	new_task->privilege = privilege;
	new_task->type = type;
	
	new_task->state = TASK_RUNNING;
	new_task->priority = priority;
	switch(new_task->priority)
	{
		case PRIO_DEAD:
			new_task->time_to_run = 0;
		break;
		case PRIO_IDLE:
			new_task->time_to_run = 3;
		break;
		case PRIO_LOW:
			new_task->time_to_run = 5;
		break;
		case PRIO_HIGH:
			new_task->time_to_run = 10;
		break;
		default:
			new_task->time_to_run = 10;
		break;
	}

	task_t *tmp_task = (task_t*)ready_queue;
	while(tmp_task->next)
	{
		tmp_task = tmp_task->next;
	}
	tmp_task->next = new_task;
	
	struct regs__ *kernel_stack = (struct regs__*)valloc(sizeof(struct regs__));
	
	u32 code_segment = 0x08, data_segment = 0x10;
	u32 eflags = 0x0202;
	kernel_stack->useresp =(u32)&exit;
	kernel_stack->ss = data_segment;
	if (new_task->privilege == 3 && new_task->type == THREAD)
        {         
           kernel_stack->ss = 0x23; 
           kernel_stack->useresp = (u32)&exit;
           code_segment = 0x1B; 
        }
	if(new_task->privilege == 3 && new_task->type == VM86)
	{
		code_segment = 0;
		kernel_stack->ss = 0x23;

		kernel_stack->useresp = (u32)&exit;
		eflags = 0x20202;
	}	
		
	kernel_stack->eflags = eflags;
	kernel_stack->cs = code_segment;
	kernel_stack->eip = (u32)entry;
	kernel_stack->err_code = 0;
	kernel_stack->int_no = 0;
	kernel_stack->eax = argc;
	kernel_stack->ecx = (uintptr_t)argv;
	kernel_stack->edx = 0;
	kernel_stack->ebx = 0;
	kernel_stack->ebp = 0;
	kernel_stack->esi = 0;
	kernel_stack->edi = 0;

	if(privilege == 3) data_segment = 0x23;
		kernel_stack->ds = data_segment;
	kernel_stack->es = data_segment;
	kernel_stack->fs = data_segment;
	kernel_stack->gs = data_segment;
	new_task->syscall_registers = (struct regs__*)kernel_stack;
	new_task->eip = (u32)entry;
	new_task->esp = (u32)kernel_stack; 
	new_task->id = pid++;
	
	task_switching = true;
	insert_current_task(new_task);
	__asm__ __volatile__("sti");	
}

void _get_task_stackFORK(task_t *new_task,void (*entry)(),size_t argc, char** argv,u8 privilege, int priority,task_type type)
{	
	__asm__ __volatile__("cli");
	
	task_switching = false;
	new_task->esp = 0;
        new_task->eip = 0;
        new_task->privilege = 0;
        new_task->state = TASK_RUNNING;
        new_task->next = 0;
        new_task->type = THREAD;
        new_task->priority = PRIO_HIGH;
        new_task->time_to_run = 10;
        new_task->ready_to_run = 1;
	new_task->wd_name = strdup("/");
	 
	tree_set_root(process_tree, (void *)new_task);
	/* Set its tree entry pointer so we can keep track
	 * of the process' entry in the process tree. */
	new_task->tree_entry = process_tree->root;
	new_task->id      = 1;       /* Init is PID 1 */
	new_task->group   = 0;
	new_task->name    = strdup("init");  /* Um, duh. */
	new_task->cmdline = NULL;
	new_task->user    = 0;       /* UID 0 */
	new_task->mask    = 022;     /* umask */
	new_task->group   = 0;       /* Task group 0 */
	//new_task->status  = 0;       /* Run status */
	new_task->fds = malloc(sizeof(fd_table_t));
	new_task->fds->refs = 1;
	new_task->fds->length   = 12;  /* Initialize the file descriptors */
	new_task->fds->capacity = 44;
	new_task->fds->entries  = malloc(sizeof(fs_node_t *) * new_task->fds->capacity);
	new_task->fds->entries[0] =  clone_fs(current_task->wd_node);
	/* Set the working directory */
	 new_task->wd_node = clone_fs(fs_root);
	new_task->wd_name = strdup("/");

	/* Heap and stack pointers (and actuals) */
	new_task->image.entry       = 0;
	new_task->image.heap        = 0;
	new_task->image.heap_actual = 0;
	//new_task->image.stack       = initial_esp + 1;
	new_task->image.user_stack  = 0;
	new_task->image.size        = 0;
	new_task->image.shm_heap    = SHM_START; /* Yeah, a bit of a hack. */

	//spin_init(new_task->image.lock);

	/* Process is not finished */
	new_task->finished = 0;
	new_task->started = 1;
	new_task->running = 1;
	new_task->wait_queue = list_create();
	new_task->shm_mappings = list_create();
	new_task->signal_queue = list_create();
	new_task->signal_kstack = NULL; /* None yet initialized */

	new_task->sched_node.prev = NULL;
	new_task->sched_node.next = NULL;
	//new_task->sched_node.value = init;

	new_task->sleep_node.prev = NULL;
	new_task->sleep_node.next = NULL;
	//new_task->sleep_node.value = init;

	new_task->timed_sleep_node = NULL;

	new_task->is_tasklet = 0;
 	tree_node_create(new_task);
	// set_process_environment(new_task, current_directory);

	/* What the hey, let's also set the description on this one */
	//new_task->description = strdup("[init]");
	  list_insert(process_list, (void *)new_task);

	new_task->fds = malloc(sizeof(fd_table_t));
	new_task->wd_node = clone_fs(current_task->wd_node);

	new_task->image.heap        = current_task->image.heap;
	new_task->image.heap_actual = current_task->image.heap_actual;
	new_task->image.size        = current_task->image.size;
 
 	new_task->wd_name = strdup(current_task->wd_name);
 
	new_task->user	 = current_task->user;
	new_task->fds->refs     = 1;
	new_task->fds->length   = current_task->fds->length;
	new_task->fds->capacity = current_task->fds->capacity;
		 
		 
 
	debug_print(INFO,"    ---");
	for (uint32_t i = 0; i < current_task->fds->length; ++i) {
			new_task->fds->entries[i] = clone_fs(current_task->fds->entries[i]);
	}
	//memset(new_task, 0, sizeof(task_t));
	new_task->kernel_stack = (u32)valloc(KERNEL_STACK_SIZE)+KERNEL_STACK_SIZE;
	
    	new_task->image.heap        = current_task->image.heap;
	new_task->image.heap_actual = current_task->image.heap_actual;
	new_task->image.size        = current_task->image.size;
	new_task->state = TASK_RUNNING;
	
		current_task->priority = priority;
	new_task->privilege = privilege;
	new_task->type = type;
	
	new_task->state = TASK_RUNNING;
	new_task->priority = priority;
	switch(new_task->priority)
	{
		case PRIO_DEAD:
			new_task->time_to_run = 0;
		break;
		case PRIO_IDLE:
			new_task->time_to_run = 3;
		break;
		case PRIO_LOW:
			new_task->time_to_run = 5;
		break;
		case PRIO_HIGH:
			new_task->time_to_run = 10;
		break;
		default:
			new_task->time_to_run = 10;
		break;
	}

       task_t *tmp_task = (task_t*)ready_queue;
	while(tmp_task->next)
	{
		tmp_task = tmp_task->next;
	}
	tmp_task->next = new_task;
	
	struct regs__ *kernel_stack = (struct regs__*)valloc(sizeof(struct regs__));
	
    u32 code_segment = 0x08, data_segment = 0x10;
    u32 eflags = 0x0202;
    kernel_stack->useresp =(u32)&exit;
    kernel_stack->ss = data_segment;
    if (new_task->privilege == 3 && new_task->type == THREAD)
        {         
           kernel_stack->ss = 0x23; 
           kernel_stack->useresp = (u32)&exit;
           code_segment = 0x1B; 
        }
	if(new_task->privilege == 3 && new_task->type == VM86)
		{
	     code_segment = 0;
	     kernel_stack->ss = 0x23;

		  kernel_stack->useresp = (u32)&exit;
		 eflags = 0x20202;
		}	
		
	kernel_stack->eflags = eflags;
	kernel_stack->cs = code_segment;
	kernel_stack->eip = (u32)entry;
	 kernel_stack->err_code = 0;
	kernel_stack->int_no = 0;
	kernel_stack->eax = argc;
	kernel_stack->ecx = (uintptr_t)argv;
	kernel_stack->edx = 0;
	kernel_stack->ebx = 0;
	kernel_stack->ebp = 0;
	kernel_stack->esi = 0;
	kernel_stack->edi = 0;

	if(privilege == 3) data_segment = 0x23;
	kernel_stack->ds = data_segment;
	kernel_stack->es = data_segment;
	kernel_stack->fs = data_segment;
	kernel_stack->gs = data_segment;
	new_task->syscall_registers = (struct regs__*)kernel_stack;
	new_task->eip = (u32)entry;


struct regs r;

	memcpy(&r, current_task->syscall_registers, sizeof(struct regs));
new_task->syscall_registers = &r;
	new_task->esp = (u32)new_task->syscall_registers; 
	new_task->id = pid++;
	
	task_switching = true;
	insert_current_task(new_task);
	__asm__ __volatile__("sti");
	
}
void create_task_vm86(void (*thread)(),char *test)
{
	task_t* new_task = malloc(sizeof(task_t));
	
	_get_task_stack(new_task,thread,0,0,3,PRIO_HIGH,VM86);	
  // sleep2(40);
}

void create_user_task(void (*thread)(),task_t* new_task,size_t argc, char** argv)
{
	//task_t* new_task = valloc(sizeof(task_t));
	_get_task_stack(new_task,thread,argc,argv,3,PRIO_HIGH,THREAD);
	//sleep2(40);
}
void create_user_taskFORK(void (*thread)(),task_t* new_task,size_t argc, char** argv)
{
	//task_t* new_task = valloc(sizeof(task_t));
	_get_task_stackFORK(new_task,thread,argc,argv,3,PRIO_HIGH,THREAD);
	//sleep2(40);
}

void create_user_task_new(void (*thread)() )
{
	 task_t* new_task = valloc(sizeof(task_t));
	_get_task_stack(new_task,thread,0,0,3,PRIO_HIGH,THREAD);
	//sleep2(40);
}
int DOTASKSWITCH = 0;
void create_task_thread(void (*thread)(),int priority)
{
  DOTASKSWITCH = 1;
	task_t* new_task = valloc(sizeof(task_t));
	_get_task_stack(new_task,thread,0,0,0,priority,THREAD);
	//sleep2(40);
}

void create_process(void (*process)(),task_type type,u8 privilege, int argc, char** argv)
{
	task_t* new_task = malloc(sizeof(task_t));
	_get_task_stack(new_task,process,argc,(uintptr_t)argv,privilege,PRIO_HIGH, type);
	//sleep2(40);	
		 
}


u32 _task_switch(u32 esp)
{

if(DOTASKSWITCH == 0)
return esp;

if(!current_task) return esp;
struct regs *r = (struct regs*)esp;
current_task->eip = r->eip;
current_task->esp = esp;
 task_t* oldTask = current_task; 
	current_task = get_current_task();
 	
 
 if(oldTask == current_task) return esp; // No task switch because old==new

if(current_task->priority == PRIO_LOW)
			current_task->time_to_run = 3;
   
        if( current_task->priority == PRIO_IDLE)
			current_task->time_to_run = 2;	
		
		 if( current_task->priority == PRIO_HIGH)
			current_task->time_to_run = 4;

for (volatile task_t *t = ready_queue; t != 0; t = t->next){
  if (t->state == TASK_SLEEPING && t->wakeup_time <= gettickcount()){
t->wakeup_time = 0;
t->state = TASK_RUNNING;
  }
  
}

while (current_task != 0 && current_task->state == TASK_SLEEPING) {
        
               current_task = current_task->next;
               
                }
if(!current_task)
{

current_task = ready_queue;

}

if (current_task == FPUTask)
 	{
 	__asm__ volatile("CLTS"); 
	}
 	else
 	{
 	u32 cr0;
 	__asm__ volatile("mov %%cr0, %0": "=r"(cr0));
 	cr0 |= BIT(3); 
	__asm__ volatile("mov %0, %%cr0":: "r"(cr0));
 	} 
	set_kernel_stack(current_task->kernel_stack+KERNEL_STACK_SIZE); //update the tss entry when we changes stack!
return current_task->esp;
}


void sleep2(u32 milliseconds) 
{
	const u32 start_ticks = gettickcount();
	u32 ticks_to_wait = milliseconds / (1000 / TIMER_HZ);

	if (ticks_to_wait == 0)
	ticks_to_wait = 1;
	
	current_task->state = TASK_SLEEPING;
	current_task->wakeup_time = start_ticks + ticks_to_wait;

	__asm__ __volatile__("int $0x20");
}


void switch_context() 
 	{
 	if(scheduler_shouldSwitchTask()) 
 	__asm__ volatile("int $0x20");
 	else
 	{
 	__asm__ volatile("hlt");
 	__asm__ volatile("int $0x20");
 	}
 	}
 



void exit()
{
    __asm__ __volatile__("cli");
	current_task->priority = PRIO_DEAD;
	current_task->time_to_run = 0;
    current_task->ready_to_run = 0;
	task_t* tmp_task = (task_t*)ready_queue;
    do
    {
        if(tmp_task->next == current_task)
        {
            tmp_task->next = current_task->next;
        }
        if(tmp_task->next)
        {
            tmp_task = tmp_task->next;
        }
    }
    while (tmp_task->next);
	delete_current_task(current_task);
  
    free((void *)((u32)current_task->kernel_stack - KERNEL_STACK_SIZE)); 
    free((void *)current_task);

    __asm__ __volatile__("sti");
	counter--;
	task_switching = 1; 
	switch_context();
}

void _exit(int status)
{
    __asm__ volatile("cli");
 current_task->priority = PRIO_DEAD;
current_task->time_to_run = 0;
     current_task->ready_to_run = 0;
      task_t* tmp_task = (task_t*)ready_queue;
    do
    {
        if(tmp_task->next == current_task)
        {
            tmp_task->next = current_task->next;
        }
        if(tmp_task->next)
        {
            tmp_task = tmp_task->next;
        }
    }
    while (tmp_task->next);
delete_current_task(current_task);
  
    //free((void *)((u32)current_task->kernel_stack - KERNEL_STACK_SIZE)); // free kernelstack
    //free((void *)current_task);

  
    
    __asm__ volatile("sti");
counter--;
task_switching = 1;
       // switch_context(); // switch to next task
    
}

void task01()
{			
	printk("Hello! from kernel task!\n");	
	//sleep2(20000);
	for(;;);
}

void task02()
{	
	printk("Hello! from user task! %d\n", getpid());
	for(;;);
}

#define COM_ENTRY (void*)0x100
#define VESA_MODE 279
 

int IdleTask(void)
{
	while(1) {printk("getpid() returns = %d \n", getpid());}
	 //	 
}

void TASK_testing()
{	 
	
	create_task_thread(IdleTask,PRIO_HIGH);
	create_task_thread(IdleTask,PRIO_HIGH);
	create_task_thread(IdleTask,PRIO_HIGH);
 
}
extern unsigned char  * lfb_vid_memory ;
extern   unsigned short term_width  ;    /* Width of the terminal (in cells) */
extern   unsigned short term_height  ;
int start_graphics_daemon()
{
 
for(;;) {memset((void *)(uintptr_t )lfb_vid_memory, 0x00, term_width * term_height * 12);}
}

void kill(int pid) {
  volatile task_t *t;
  volatile task_t *prev = ready_queue;
  for (t = ready_queue; t != 0; prev = t, t = t->next) {
    if (t->id == pid) {
        prev->next = t->next;
      counter--;
    }
  }
}

#include <fs.h>
#include <system.h>
#include <process.h>
#include <tree.h>
#include <list.h>
#include "../bitset.h"
#include <logging.h>
static bitset_t pid_set;
#include <shm.h>
task_t * process_from_pid(int pid) {
 
}
#define MAX_PID 32768

/*
 * Initialize the process tree and ready queue.
 */
void initialize_process_tree(void) {
	process_tree = tree_create();
	process_list = list_create();
	process_queue = list_create();
	sleep_queue = list_create();

	/* Start off with enough bits for 64 processes */
	bitset_init(&pid_set, MAX_PID / 8);
	/* First two bits are set by default */
	bitset_set(&pid_set, 0);
	bitset_set(&pid_set, 1);
}



task_t * spawn_process(volatile task_t * parent, int reuse_fds) {
 
	task_t * proc = malloc(sizeof(   task_t));
	proc->fds = malloc(sizeof(fd_table_t));
	proc->wd_node = clone_fs(parent->wd_node);
 
	proc->image.heap        = parent->image.heap;
	proc->image.heap_actual = parent->image.heap_actual;
	proc->image.size        = parent->image.size;
 
	proc->wd_name = strdup(parent->wd_name);
 
	proc->user	 = parent->user;
	proc->fds->refs     = 1;
	proc->fds->length   = parent->fds->length;
	proc->fds->capacity = parent->fds->capacity;
		 
	proc->fds->entries  = malloc(sizeof(fs_node_t *) *44);
 
	debug_print(INFO,"    ---");
	for (uint32_t i = 0; i < parent->fds->length; ++i) {
		proc->fds->entries[i] = clone_fs(parent->fds->entries[i]);
	}
 
	return proc;
}
extern task_t * copytask;
extern   int returntouserspace  ;
extern int return_to_userspace ;
extern page_directory_t * current_directory;

uint32_t fork(void) {
	uintptr_t esp, ebp;
 
	task_t * parent = (task_t *)current_task;

	page_directory_t * directory = clone_directory(current_directory);
	switch_page_directory(directory);
 
	task_t * new_task = malloc(sizeof(task_t));
	set_process_environment(new_task, directory);
	create_user_taskFORK(&return_to_userspace , new_task,current_task->syscall_registers->eax,current_task->syscall_registers->ecx);
	 
	printk("new_task->id= %d\n", new_task->id);
	/* Return the child PID */
	return 0;
}

static int wait_candidate(task_t * parent, int pid, int options, task_t * proc) {
	(void)options; /* there is only one option that affects candidacy, and we don't support it yet */

	if (!proc) return 0;

	if (pid < -1) {
		if (proc->group == -pid || proc->id == -pid) return 1;
	} else if (pid == 0) {
		/* Matches our group ID */
		if (proc->group == parent->id) return 1;
	} else if (pid > 0) {
		/* Specific pid */
		if (proc->id == pid) return 1;
	} else {
		return 1;
	}
	return 0;
}
int wakeup_queue(list_t * queue) {
	int awoken_processes = 0;
	while (queue->length > 0) {
		//spin_lock(wait_lock_tmp);
		node_t * node = list_pop(queue);
		//spin_unlock(wait_lock_tmp);
		if (!((task_t *)node->value)->finished) {
			make_process_ready(node->value);
		}
		awoken_processes++;
	}
	return awoken_processes;
}

int wakeup_queue_interrupted(list_t * queue) {
	int awoken_processes = 0;
	while (queue->length > 0) {
	//	spin_lock(wait_lock_tmp);
		node_t * node = list_pop(queue);
	//	spin_unlock(wait_lock_tmp);
		if (!((task_t *)node->value)->finished) {
			task_t * proc = node->value;
			proc->sleep_interrupted = 1;
			make_process_ready(proc);
		}
		awoken_processes++;
	}
	return awoken_processes;
}
void make_process_ready(task_t * proc) {
	if (proc->sleep_node.owner != NULL) {
		if (proc->sleep_node.owner == sleep_queue) {
			/* XXX can't wake from timed sleep */
			if (proc->timed_sleep_node) {
				IRQ_OFF;
				//spin_lock(sleep_lock);
				list_delete(sleep_queue, proc->timed_sleep_node);
				//spin_unlock(sleep_lock);
				IRQ_RES;
				proc->sleep_node.owner = NULL;
				free(proc->timed_sleep_node->value);
			}
			/* Else: I have no idea what happened. */
		} else {
			proc->sleep_interrupted = 1;
			//spin_lock(wait_lock_tmp);
			list_delete((list_t*)proc->sleep_node.owner, &proc->sleep_node);
			//spin_unlock(wait_lock_tmp);
		}
	}
	if (proc->sched_node.owner) {
		debug_print(WARNING, "Can't make process ready without removing from owner list: %d", proc->id);
		debug_print(WARNING, "  (This is a bug) Current owner list is 0x%x (ready queue is 0x%x)", proc->sched_node.owner, process_queue);
		return;
	}
	//spin_lock(process_queue_lock);
	list_append(process_queue, &proc->sched_node);
	//spin_unlock(process_queue_lock);
}

int sleep_on(list_t * queue) {
	if (current_task->sleep_node.owner) {
		/* uh, we can't sleep right now, we're marked as ready */
		_task_switch(0);
		return 0;
	}
	current_task->sleep_interrupted = 0;
	///spin_lock(wait_lock_tmp);
	list_append(queue, (node_t *)&current_task->sleep_node);
	//spin_unlock(wait_lock_tmp);
	_task_switch(0);
	return current_task->sleep_interrupted;
}



int waitpid(int pid, int * status, int options) {
	task_t * proc = (task_t *)current_task;
	if (proc->group) {
		proc = process_from_pid(proc->group);
	}

	debug_print(INFO, "waitpid(%s%d, ..., %d) (from pid=%d.%d)", (pid >= 0) ? "" : "-", (pid >= 0) ? pid : -pid, options, current_task->id, current_task->group);

	do {
		task_t * candidate = NULL;
		int has_children = 0;

		/* First, find out if there is anyone to reap */
		foreach(node, proc->tree_entry->children) {
			if (!node->value) {
				continue;
			}
			task_t * child = ((tree_node_t *)node->value)->value;

			if (wait_candidate(proc, pid, options, child)) {
				has_children = 1;
				if (child->finished) {
					candidate = child;
					break;
				}
			}
		}

		if (!has_children) {
			/* No valid children matching this description */
			debug_print(INFO, "No children matching description.");
			return -ECHILD;
		}

		if (candidate) {
			debug_print(INFO, "Candidate found (%x:%d), bailing early.", candidate, candidate->id);
			if (status) {
				*status = candidate->status;
			}
			int pid = candidate->id;
			//reap_process(candidate);
			return pid;
		} else {
			if (options & 1) {
				return 0;
			}
			debug_print(INFO, "Sleeping until queue is done.");
			/* Wait */
			if (sleep_on(proc->wait_queue) != 0) {
				debug_print(INFO, "wait() was interrupted");
				return -EINTR;
			}
		}
	} while (1);
}


uint32_t process_append_fd(task_t * proc, fs_node_t * node) {
	/* Fill gaps */

	for (unsigned int i = 0; i < proc->fds->length; ++i) {
 
		if (!proc->fds->entries[i]) {
			proc->fds->entries[i] = node;
printk("proc->fds->length = %d \n", i);
			return i;
		}
	}

	/* No gaps, expand */
	if (proc->fds->length == proc->fds->capacity) {
		proc->fds->capacity *= 2;
		proc->fds->entries = realloc(proc->fds->entries, sizeof(fs_node_t *) * proc->fds->capacity);
	}
	proc->fds->entries[proc->fds->length] = node;
	proc->fds->length++;
	printk("proc->fds->length = %d \n", proc->fds->length);
	return proc->fds->length-1;
}

/*
 * clone the current thread and create a new one in the same
 * memory space with the given pointer as its new stack.
 */
uint32_t
clone(uintptr_t new_stack, uintptr_t thread_func, uintptr_t arg) {
	uintptr_t esp, ebp;

	current_task->syscall_registers->eax = 0;

	/* Make a pointer to the parent process (us) on the stack */
	task_t * parent = (task_t *)current_task;
	assert(parent && "Cloned from nothing??");
	page_directory_t * directory = current_directory;
	/* Spawn a new process from this one */
	task_t * new_proc = spawn_process(current_task, 1);
	assert(new_proc && "Could not allocate a new process!");
	/* Set the new process' page directory to the original process' */
	//set_process_environment(new_proc, directory);
	directory->ref_count++;
	/* Read the instruction pointer */

	struct regs r;
	memcpy(&r, current_task->syscall_registers, sizeof(struct regs));
	new_proc->syscall_registers = &r;

	esp = new_proc->image.stack;
	ebp = esp;

	/* Set the gid */
	if (current_task->group) {
		new_proc->group = current_task->group;
	} else {
		/* We are the session leader */
		new_proc->group = current_task->id;
	}

	new_proc->syscall_registers->ebp = new_stack;
	new_proc->syscall_registers->eip = thread_func;

 
	new_proc->syscall_registers->useresp = new_stack;
 
	new_proc->thread.ebp = ebp;

	new_proc->is_tasklet = parent->is_tasklet;
 
	make_process_ready(new_proc);
 
	return new_proc->id;
}
/*
 * Get the next available PID
 *
 * @return A usable PID for a new process.
 */
static int _next_pid = 2;
int get_next_pid(void) {
	if (_next_pid > MAX_PID) {
		int index = bitset_ffub(&pid_set);
		/*
		 * Honestly, we don't have the memory to really risk reaching
		 * the point where we have MAX_PID processes running
		 * concurrently, so this assertion should be "safe enough".
		 */
		assert(index != -1);
		bitset_set(&pid_set, index);
		return index;
	}
	int pid = _next_pid;
	_next_pid++;
	assert(!bitset_test(&pid_set, pid) && "Next PID already allocated?");
	bitset_set(&pid_set, pid);
	return pid;
}
#define USER_STACK_BOTTOM 0xAFF00000
void release_directory_for_exec(page_directory_t * dir) {
	uint32_t i;
	/* This better be the only owner of this directory... */
	for (i = 0; i < 1024; ++i) {
		if (!dir->tables[i] || (uintptr_t)dir->tables[i] == (uintptr_t)0xFFFFFFFF) {
			continue;
		}
		if (kernel_directory->tables[i] != dir->tables[i]) {
			if (i * 0x1000 * 1024 < USER_STACK_BOTTOM) {
				for (uint32_t j = 0; j < 1024; ++j) {
					if (dir->tables[i]->pages[j].frame) {
						free_frame(&(dir->tables[i]->pages[j]));
					}
				}
				dir->physical_tables[i] = 0;
				free(dir->tables[i]);
				dir->tables[i] = 0;
			}
		}
	}
}

volatile task_t * _task_initialize_random(volatile task_t * TASK)
{ 
	TASK   = (task_t*)valloc(sizeof(task_t));

	memset((task_t *)TASK, 0, sizeof(task_t));
	TASK->id = pid++;
	TASK->esp = 0;
	TASK->eip = 0;
	TASK->privilege = 0;
	TASK->state = TASK_RUNNING;
	TASK->next = 0;
	TASK->type = THREAD;
	TASK->priority = PRIO_HIGH;
	TASK->time_to_run = 10;
	TASK->ready_to_run = 1;
	TASK->wd_name = strdup("/");
	TASK->kernel_stack = (u32)valloc(KERNEL_STACK_SIZE)+KERNEL_STACK_SIZE;
	 
	TASK->tree_entry = process_tree->root;
	TASK->id      = 1;       /* Init is PID 1 */
	TASK->group   = 0;
	TASK->name    = strdup("init");  /* Um, duh. */
	TASK->cmdline = NULL;
	TASK->user    = 0;       /* UID 0 */
	TASK->mask    = 022;     /* umask */
	TASK->group   = 0;       /* Task group 0 */
	//TASK->status  = 0;       /* Run status */
	TASK->fds = malloc(sizeof(fd_table_t));
	TASK->fds->refs = 1;
	TASK->fds->length   = 44;  /* Initialize the file descriptors */
	TASK->fds->capacity = 44;
	TASK->fds->entries  = malloc(sizeof(fs_node_t *) * TASK->fds->capacity);

	TASK->wd_name = strdup("/");

	TASK->finished = 0;
	TASK->started = 1;
	TASK->running = 1;
	TASK->wait_queue = list_create();
	TASK->shm_mappings = list_create();
	TASK->signal_queue = list_create();
	TASK->signal_kstack = NULL; /* None yet initialized */

	TASK->sched_node.prev = NULL;
	TASK->sched_node.next = NULL;
	//TASK->sched_node.value = init;

	TASK->sleep_node.prev = NULL;
	TASK->sleep_node.next = NULL;
	//TASK->sleep_node.value = init;

	TASK->timed_sleep_node = NULL;

	TASK->is_tasklet = 0;
return TASK;
 

	 
}

void set_process_environment(task_t * proc, page_directory_t * directory) {
	assert(proc);
	assert(directory);

	proc->thread.page_directory = directory;
}
int process_wait_nodes( task_t * process,fs_node_t * nodes[], int timeout) {
	 
}

//void make_process_ready(task_t * proc) {
	 
//}
void switch_next() {}


int process_awaken_from_fswait(task_t * process, int index) {
	 
}

int process_is_ready(task_t * proc) {
	 
}

void sleep_until(task_t * process, unsigned long seconds, unsigned long subseconds) {
}
//void switch_task() {}
