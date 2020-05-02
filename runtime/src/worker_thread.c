// Something is not idempotent with this or some other include.
// If placed in Local Includes, error is triggered that memset was implicitly declared
#include <runtime.h>

/***************************
 * External Includes       *
 **************************/
#include <pthread.h>  // POSIX Threads
#include <signal.h>   // POSIX Signals
#include <sched.h>    // Wasmception. Included as submodule
#include <sys/mman.h> // Wasmception. Included as submodule
#include <uv.h>       // Libuv

/***************************
 * Local Includes          *
 **************************/
#include <current_sandbox.h>
#include <sandbox_completion_queue.h>
#include <sandbox_request_scheduler.h>
#include <sandbox_run_queue.h>
#include <sandbox_run_queue_fifo.h>
#include <sandbox_run_queue_ps.h>
#include <types.h>
#include <worker_thread.h>

/***************************
 * Worker Thread State     *
 **************************/

// context pointer to switch to when this thread gets a SIGUSR1
__thread arch_context_t *worker_thread_next_context = NULL;

// context of the runtime thread before running sandboxes or to resume its "main".
__thread arch_context_t worker_thread_base_context;

// libuv i/o loop handle per sandboxing thread!
__thread uv_loop_t worker_thread_uvio_handle;

// Flag to signify if the thread is currently running callbacks in the libuv event loop
static __thread bool worker_thread_is_in_callback;

/**************************************************
 * Worker Thread Logic
 *************************************************/

/**
 * @brief Switches to the next sandbox, placing the current sandbox of the completion queue if in RETURNED state
 * @param next The Sandbox Context to switch to or NULL
 * @return void
 */
static inline void
worker_thread_switch_to_sandbox(struct sandbox *next_sandbox)
{
	arch_context_t *next_register_context = next_sandbox == NULL ? NULL : &next_sandbox->ctxt;

	software_interrupt_disable();

	// Get the old sandbox we're switching from
	struct sandbox *previous_sandbox          = current_sandbox_get();
	arch_context_t *previous_register_context = previous_sandbox == NULL ? NULL : &previous_sandbox->ctxt;

	// Set the current sandbox to the next
	current_sandbox_set(next_sandbox);

	// and switch to the associated context. But what is the purpose of worker_thread_next_context?
	worker_thread_next_context = next_register_context;
	arch_context_switch(previous_register_context, next_register_context);

	// If the current sandbox we're switching from is in a RETURNED state, add to completion queue
	if (previous_sandbox != NULL && previous_sandbox->state == RETURNED)
		sandbox_completion_queue_add(previous_sandbox);

	software_interrupt_enable();
}

/**
 * Mark a blocked sandbox as runnable and add it to the runqueue
 * @param sandbox the sandbox to check and update if blocked
 **/
void
worker_thread_wakeup_sandbox(sandbox_t *sandbox)
{
	software_interrupt_disable();
	// debuglog("[%p: %s]\n", sandbox, sandbox->module->name);
	if (sandbox->state == BLOCKED) {
		sandbox->state = RUNNABLE;
		sandbox_run_queue_add(sandbox);
	}
	software_interrupt_enable();
}


/**
 * Mark the currently executing sandbox as blocked, remove it from the local runqueue, and pull the sandbox at the head
 *of the runqueue
 **/
void
worker_thread_block_current_sandbox(void)
{
	assert(worker_thread_is_in_callback == false);
	software_interrupt_disable();

	// Remove the sandbox we were just executing from the runqueue and mark as blocked
	struct sandbox *previous_sandbox = current_sandbox_get();
	sandbox_run_queue_delete(previous_sandbox);
	previous_sandbox->state = BLOCKED;

	// Switch to the next sandbox
	struct sandbox *next_sandbox = sandbox_run_queue_get_next();
	debuglog("[%p: %next_sandbox, %p: %next_sandbox]\n", previous_sandbox, previous_sandbox->module->name,
	         next_sandbox, next_sandbox ? next_sandbox->module->name : "");
	software_interrupt_enable();
	worker_thread_switch_to_sandbox(next_sandbox);
}


/**
 * Execute I/O
 **/
void
worker_thread_process_io(void)
{
#ifdef USE_HTTP_UVIO
#ifdef USE_HTTP_SYNC
	// realistically, we're processing all async I/O on this core when a sandbox blocks on http processing, not
	// great! if there is a way (TODO), perhaps RUN_ONCE and check if your I/O is processed, if yes, return else do
	// async block!
	uv_run(worker_thread_get_libuv_handle(), UV_RUN_DEFAULT);
#else  /* USE_HTTP_SYNC */
	worker_thread_block_current_sandbox();
#endif /* USE_HTTP_UVIO */
#else
	assert(false);
	// it should not be called if not using uvio for http
#endif
}

/**
 * TODO: What is this doing?
 **/
void __attribute__((noinline)) __attribute__((noreturn)) worker_thread_sandbox_switch_preempt(void)
{
	pthread_kill(pthread_self(), SIGUSR1);

	assert(false); // should not get here..
	while (true)
		;
}

/**
 * Run all outstanding events in the local thread's libuv event loop
 **/
void
worker_thread_execute_libuv_event_loop(void)
{
	worker_thread_is_in_callback = true;
	int n = uv_run(worker_thread_get_libuv_handle(), UV_RUN_NOWAIT), i = 0;
	while (n > 0) {
		n--;
		uv_run(worker_thread_get_libuv_handle(), UV_RUN_NOWAIT);
	}
	worker_thread_is_in_callback = false;
}

/**
 * The entry function for sandbox worker threads
 * Initializes thread-local state, unmasks signals, sets up libuv loop and
 * @param return_code - argument provided by pthread API. We set to -1 on error
 **/
void *
worker_thread_main(void *return_code)
{
	// Initialize Worker State
	arch_context_init(&worker_thread_base_context, 0, 0);

	sandbox_run_queue_fifo_initialize();
	// sandbox_run_queue_ps_initialize();

	sandbox_completion_queue_initialize();
	software_interrupt_is_disabled = false;
	worker_thread_next_context     = NULL;
#ifndef PREEMPT_DISABLE
	software_interrupt_unmask_signal(SIGALRM);
	software_interrupt_unmask_signal(SIGUSR1);
#endif
	uv_loop_init(&worker_thread_uvio_handle);
	worker_thread_is_in_callback = false;


	// Begin Worker Execution Loop
	struct sandbox *next_sandbox;
	while (true) {
		assert(current_sandbox_get() == NULL);
		// If "in a callback", the libuv event loop is triggering this, so we don't need to start it
		if (!worker_thread_is_in_callback) worker_thread_execute_libuv_event_loop();

		software_interrupt_disable();
		next_sandbox = sandbox_run_queue_get_next();
		software_interrupt_enable();

		if (next_sandbox != NULL) {
			worker_thread_switch_to_sandbox(next_sandbox);
			sandbox_completion_queue_free(1);
		}
	}

	*(int *)return_code = -1;
	pthread_exit(return_code);
}

/**
 * Called when the function in the sandbox exits
 * Removes the standbox from the thread-local runqueue, sets its state to RETURNED,
 * releases the linear memory, and then switches to the sandbox at the head of the runqueue
 * TODO: Consider moving this to a future current_sandbox file. This has thus far proven difficult to move
 **/
void
worker_thread_exit_current_sandbox(void)
{
	// Remove the sandbox that exited from the runqueue and set state to RETURNED
	struct sandbox *previous_sandbox = current_sandbox_get();
	assert(previous_sandbox);
	software_interrupt_disable();
	sandbox_run_queue_delete(previous_sandbox);
	previous_sandbox->state = RETURNED;

	struct sandbox *next_sandbox = sandbox_run_queue_get_next();
	assert(next_sandbox != previous_sandbox);
	software_interrupt_enable();
	// Because the stack is still in use, only unmap linear memory and defer free resources until "main
	// function execution"
	munmap(previous_sandbox->linear_memory_start, SBOX_MAX_MEM + PAGE_SIZE);
	worker_thread_switch_to_sandbox(next_sandbox);
}
