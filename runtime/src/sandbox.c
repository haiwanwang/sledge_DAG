#include <assert.h>
#include <runtime.h>
#include <worker_thread.h>
#include <sandbox.h>
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <uv.h>
#include <libuv_callbacks.h>
#include <current_sandbox.h>
#include <http_parser_settings.h>

/**
 * Takes the arguments from the sandbox struct and writes them into the WebAssembly linear memory
 * TODO: why do we have to pass argument count explicitly? Can't we just get this off the sandbox?
 * @param argument_count
 **/
static inline void
current_sandbox_setup_arguments(i32 argument_count)
{
	struct sandbox *curr      = current_sandbox_get();
	char *          arguments = current_sandbox_get_arguments();

	// whatever gregor has, to be able to pass arguments to a module!
	curr->arguments_offset = sandbox_lmbound;
	assert(sandbox_lmbase == curr->linear_memory_start);
	expand_memory();

	i32 *array_ptr  = worker_thread_get_memory_ptr_void(curr->arguments_offset, argument_count * sizeof(i32));
	i32  string_off = curr->arguments_offset + (argument_count * sizeof(i32));

	for (int i = 0; i < argument_count; i++) {
		char * arg    = arguments + (i * MODULE_MAX_ARGUMENT_SIZE);
		size_t str_sz = strlen(arg) + 1;

		array_ptr[i] = string_off;
		// why get_memory_ptr_for_runtime??
		strncpy(get_memory_ptr_for_runtime(string_off, str_sz), arg, strlen(arg));

		string_off += str_sz;
	}
	stub_init(string_off);
}

/**
 * Run the http-parser on the sandbox's request_response_data using the configured settings global
 * @param sandbox the sandbox containing the req_resp data that we want to parse
 * @param length The size of the request_response_data that we want to parse
 * @returns 0
 *
 **/
int
sandbox_parse_http_request(struct sandbox *sandbox, size_t length)
{
	// Why is our start address sandbox->request_response_data + sandbox->request_response_data_length?
	// it's like a cursor to keep track of what we've read so far
	http_parser_execute(&sandbox->http_parser, http_parser_settings_get(),
	                    sandbox->request_response_data + sandbox->request_response_data_length, length);
	return 0;
}


/**
 * Receive and Parse the Request for the current sandbox
 * @return 1 on success, 0 if no context, < 0 on failure.
 **/
static inline int
current_sandbox_receive_and_parse_client_request(void)
{
	struct sandbox *curr               = current_sandbox_get();
	curr->request_response_data_length = 0;
#ifndef USE_HTTP_UVIO
	int r = 0;
	r     = recv(curr->client_socket_descriptor, (curr->request_response_data), curr->module->max_request_size, 0);
	if (r <= 0) {
		if (r < 0) perror("recv1");
		return r;
	}
	while (r > 0) {
		if (current_sandbox_parse_http_request(r) != 0) return -1;
		curr->request_response_data_length += r;
		struct http_request *rh = &curr->http_request;
		if (rh->message_end) break;

		r = recv(curr->client_socket_descriptor,
		         (curr->request_response_data + curr->request_response_data_length),
		         curr->module->max_request_size - curr->request_response_data_length, 0);
		if (r < 0) {
			perror("recv2");
			return r;
		}
	}
#else
	int r = uv_read_start((uv_stream_t *)&curr->client_libuv_stream,
	                      libuv_callbacks_on_allocate_setup_request_response_data,
	                      libuv_callbacks_on_read_parse_http_request);
	worker_thread_process_io();
	if (curr->request_response_data_length == 0) return 0;
#endif
	return 1;
}

/**
 * Sends Response Back to Client
 * @return RC. -1 on Failure
 **/
static inline int
current_sandbox_build_and_send_client_response(void)
{
	int             sndsz                  = 0;
	struct sandbox *curr                   = current_sandbox_get();
	int             response_header_length = strlen(HTTP_RESPONSE_200_OK) + strlen(HTTP_RESPONSE_CONTENT_TYPE)
	                             + strlen(HTTP_RESPONSE_CONTENT_LENGTH);
	int body_length = curr->request_response_data_length - response_header_length;

	memset(curr->request_response_data, 0,
	       strlen(HTTP_RESPONSE_200_OK) + strlen(HTTP_RESPONSE_CONTENT_TYPE)
	         + strlen(HTTP_RESPONSE_CONTENT_LENGTH));
	strncpy(curr->request_response_data, HTTP_RESPONSE_200_OK, strlen(HTTP_RESPONSE_200_OK));
	sndsz += strlen(HTTP_RESPONSE_200_OK);

	if (body_length == 0) goto done;
	strncpy(curr->request_response_data + sndsz, HTTP_RESPONSE_CONTENT_TYPE, strlen(HTTP_RESPONSE_CONTENT_TYPE));
	if (strlen(curr->module->response_content_type) <= 0) {
		strncpy(curr->request_response_data + sndsz + strlen("Content-type: "),
		        HTTP_RESPONSE_CONTENT_TYPE_PLAIN, strlen(HTTP_RESPONSE_CONTENT_TYPE_PLAIN));
	} else {
		strncpy(curr->request_response_data + sndsz + strlen("Content-type: "),
		        curr->module->response_content_type, strlen(curr->module->response_content_type));
	}
	sndsz += strlen(HTTP_RESPONSE_CONTENT_TYPE);
	char len[10] = { 0 };
	sprintf(len, "%d", body_length);
	strncpy(curr->request_response_data + sndsz, HTTP_RESPONSE_CONTENT_LENGTH,
	        strlen(HTTP_RESPONSE_CONTENT_LENGTH));
	strncpy(curr->request_response_data + sndsz + strlen("Content-length: "), len, strlen(len));
	sndsz += strlen(HTTP_RESPONSE_CONTENT_LENGTH);
	sndsz += body_length;

done:
	assert(sndsz == curr->request_response_data_length);
	// Get End Timestamp
	u64 end_time     = __getcycles();
	curr->total_time = end_time - curr->start_time;
	// TODO: Refactor to log file
	printf("%s():%d, %d, %lu\n", curr->module->name, curr->module->port, curr->module->relative_deadline_us,
	       (uint64_t)(curr->total_time / runtime_processor_speed_MHz));
	// if (end_time < curr->absolute_deadline) {
	// 	printf("meadDeadline Met with %f us to spare\n",
	// 	       (curr->absolute_deadline - end_time) / runtime_processor_speed_MHz);
	// } else {
	// 	printf("Deadline NOT MET! Overran by %f us\n",
	// 	       (end_time - curr->absolute_deadline) / runtime_processor_speed_MHz);
	// }

#ifndef USE_HTTP_UVIO
	int r = send(curr->client_socket_descriptor, curr->request_response_data, sndsz, 0);
	if (r < 0) {
		perror("send");
		return -1;
	}
	while (r < sndsz) {
		int s = send(curr->client_socket_descriptor, curr->request_response_data + r, sndsz - r, 0);
		if (s < 0) {
			perror("send");
			return -1;
		}
		r += s;
	}
#else
	uv_write_t req = {
		.data = curr,
	};
	uv_buf_t bufv = uv_buf_init(curr->request_response_data, sndsz);
	int      r    = uv_write(&req, (uv_stream_t *)&curr->client_libuv_stream, &bufv, 1,
                         libuv_callbacks_on_write_wakeup_sandbox);
	worker_thread_process_io();
#endif
	return 0;
}

/**
 * Sandbox execution logic
 * Handles setup, request parsing, WebAssembly initialization, function execution, response building and sending, and
 *cleanup
 **/
void
current_sandbox_main(void)
{
	struct sandbox *current_sandbox = current_sandbox_get();
	// FIXME: is this right? this is the first time this sandbox is running.. so it wont
	//        return to worker_thread_switch_to_sandbox() api..
	//        we'd potentially do what we'd in worker_thread_switch_to_sandbox() api here for cleanup..
	if (software_interrupt_is_enabled() == false) {
		arch_context_init(&current_sandbox->ctxt, 0, 0);
		worker_thread_next_context = NULL;
		software_interrupt_enable();
	}
	struct module *current_module = sandbox_get_module(current_sandbox);
	int            argument_count = module_get_argument_count(current_module);
	// for stdio

	// Try to initialize file descriptors 0, 1, and 2 as io handles 0, 1, 2
	// We need to check that we get what we expect, as these IO handles may theoretically have been taken
	// TODO: why do the file descriptors have to match the io handles?
	int f = current_sandbox_initialize_io_handle_and_set_file_descriptor(0);
	assert(f == 0);
	f = current_sandbox_initialize_io_handle_and_set_file_descriptor(1);
	assert(f == 1);
	f = current_sandbox_initialize_io_handle_and_set_file_descriptor(2);
	assert(f == 2);

	// Initialize the HTTP-Parser for a request
	http_parser_init(&current_sandbox->http_parser, HTTP_REQUEST);

	// Set the current_sandbox as the data the http-parser has access to
	current_sandbox->http_parser.data = current_sandbox;

	// NOTE: if more headers, do offset by that!
	int response_header_length = strlen(HTTP_RESPONSE_200_OK) + strlen(HTTP_RESPONSE_CONTENT_TYPE)
	                             + strlen(HTTP_RESPONSE_CONTENT_LENGTH);

#ifdef USE_HTTP_UVIO

	// Initialize libuv TCP stream
	int r = uv_tcp_init(worker_thread_get_libuv_handle(), (uv_tcp_t *)&current_sandbox->client_libuv_stream);
	assert(r == 0);

	// Set the current sandbox as the data the libuv callbacks have access to
	current_sandbox->client_libuv_stream.data = current_sandbox;

	// Open the libuv TCP stream
	r = uv_tcp_open((uv_tcp_t *)&current_sandbox->client_libuv_stream, current_sandbox->client_socket_descriptor);
	assert(r == 0);
#endif

	// If the HTTP Request returns 1, we've successfully received and parsed the HTTP request, so execute it!
	if (current_sandbox_receive_and_parse_client_request() > 0) {
		//
		current_sandbox->request_response_data_length = response_header_length;

		// Allocate the WebAssembly Sandbox
		alloc_linear_memory();
		module_initialize_globals(current_module);
		module_initialize_memory(current_module);

		// Copy the arguments into the WebAssembly sandbox
		current_sandbox_setup_arguments(argument_count);

		// Executing the function within the WebAssembly sandbox
		current_sandbox->return_value = module_main(current_module, argument_count,
		                                            current_sandbox->arguments_offset);

		// Retrieve the result from the WebAssembly sandbox, construct the HTTP response, and send to client
		current_sandbox_build_and_send_client_response();
	}

	// Cleanup connection and exit sandbox

#ifdef USE_HTTP_UVIO
	uv_close((uv_handle_t *)&current_sandbox->client_libuv_stream, libuv_callbacks_on_close_wakeup_sakebox);
	worker_thread_process_io();
#else
	close(current_sandbox->client_socket_descriptor);
#endif
	worker_thread_exit_current_sandbox();
}

/**
 * Allocates the memory for a sandbox to run a module
 * @param module the module that we want to run
 * @returns the resulting sandbox or NULL if mmap failed
 **/
static inline struct sandbox *
sandbox_allocate_memory(struct module *module)
{
	unsigned long memory_size = SBOX_MAX_MEM; // 4GB

	// Why do we add max_request_or_response_size?
	unsigned long sandbox_size       = sizeof(struct sandbox) + module->max_request_or_response_size;
	unsigned long linear_memory_size = WASM_PAGE_SIZE * WASM_START_PAGES;

	if (linear_memory_size + sandbox_size > memory_size) return NULL;
	assert(round_up_to_page(sandbox_size) == sandbox_size);

	// What does mmap do exactly with file_descriptor -1?
	void *addr = mmap(NULL, sandbox_size + memory_size + /* guard page */ PAGE_SIZE, PROT_NONE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) return NULL;

	void *addr_rw = mmap(addr, sandbox_size + linear_memory_size, PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (addr_rw == MAP_FAILED) {
		munmap(addr, memory_size + PAGE_SIZE);
		return NULL;
	}

	struct sandbox *sandbox = (struct sandbox *)addr;
	// can it include sandbox as well?
	sandbox->linear_memory_start = (char *)addr + sandbox_size;
	sandbox->linear_memory_size  = linear_memory_size;
	sandbox->module              = module;
	sandbox->sandbox_size        = sandbox_size;
	module_acquire(module);

	return sandbox;
}

struct sandbox *
sandbox_allocate(sandbox_request_t *sandbox_request)
{
	struct module *        module            = sandbox_request->module;
	char *                 arguments         = sandbox_request->arguments;
	int                    socket_descriptor = sandbox_request->socket_descriptor;
	const struct sockaddr *socket_address    = sandbox_request->socket_address;
	u64                    start_time        = sandbox_request->start_time;
	u64                    absolute_deadline = sandbox_request->absolute_deadline;

	if (!module_is_valid(module)) return NULL;

	// FIXME: don't use malloc. huge security problem!
	// perhaps, main should be in its own sandbox, when it is not running any sandbox.
	struct sandbox *sandbox = (struct sandbox *)sandbox_allocate_memory(module);
	if (!sandbox) return NULL;

	// Assign the start time from the request
	sandbox->start_time        = start_time;
	sandbox->absolute_deadline = absolute_deadline;

	// actual module instantiation!
	sandbox->arguments   = (void *)arguments;
	sandbox->stack_size  = module->stack_size;
	sandbox->stack_start = mmap(NULL, sandbox->stack_size, PROT_READ | PROT_WRITE,
	                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
	if (sandbox->stack_start == MAP_FAILED) {
		perror("mmap");
		assert(0);
	}
	sandbox->client_socket_descriptor = socket_descriptor;
	if (socket_address) memcpy(&sandbox->client_address, socket_address, sizeof(struct sockaddr));
	for (int i = 0; i < SANDBOX_MAX_IO_HANDLE_COUNT; i++) sandbox->io_handles[i].file_descriptor = -1;
	ps_list_init_d(sandbox);

	// Setup the sandbox's context, stack, and instruction pointer
	arch_context_init(&sandbox->ctxt, (reg_t)current_sandbox_main,
	                  (reg_t)(sandbox->stack_start + sandbox->stack_size));
	return sandbox;
}

/**
 * Free stack and heap resources.. also any I/O handles.
 * @param sandbox
 **/
void
sandbox_free(struct sandbox *sandbox)
{
	// you have to context switch away to free a sandbox.
	if (!sandbox || sandbox == current_sandbox_get()) return;

	// again sandbox should be done and waiting for the parent.
	if (sandbox->state != RETURNED) return;

	int sz = sizeof(struct sandbox);

	sz += sandbox->module->max_request_or_response_size;
	module_release(sandbox->module);

	void * stkaddr = sandbox->stack_start;
	size_t stksz   = sandbox->stack_size;

	// depending on the memory type
	// free_linear_memory(sandbox->linear_memory_start, sandbox->linear_memory_size,
	// sandbox->linear_memory_max_size);

	int ret;
	// mmaped memory includes sandbox structure in there.
	ret = munmap(sandbox, sz);
	if (ret) perror("munmap sandbox");

	// remove stack!
	// for some reason, removing stack seem to cause crash in some cases.
	ret = munmap(stkaddr, stksz);
	if (ret) perror("munmap stack");
}
