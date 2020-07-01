#ifndef SFRT_MODULE_H
#define SFRT_MODULE_H

#include <uv.h>

#include <types.h>

struct module {
	char                        name[MODULE_MAX_NAME_LENGTH];
	char                        path[MODULE_MAX_PATH_LENGTH];
	void *                      dynamic_library_handle; /* Handle to the *.so of the serverless function */
	i32                         argument_count;
	u32                         stack_size; /* a specification? */
	u64                         max_memory; /* perhaps a specification of the module. (max 4GB) */
	u32                         relative_deadline_us;
	u32                         reference_count; /* ref count how many instances exist here. */
	struct indirect_table_entry indirect_table[INDIRECT_TABLE_SIZE];
	struct sockaddr_in          socket_address;
	int                         socket_descriptor;
	int                         port;

	/*
	 * unfortunately, using UV for accepting connections is not great!
	 * on_connection, to create a new accepted connection, will have to init a tcp handle,
	 * which requires a uvloop. cannot use main as rest of the connection is handled in
	 * sandboxing threads, with per-core(per-thread) tls data-structures.
	 * so, using direct epoll for accepting connections.
	 */

	// TODO: Should this be removed?
	//	uv_handle_t srvuv;

	unsigned long max_request_size;
	char          request_headers[HTTP_MAX_HEADER_COUNT][HTTP_MAX_HEADER_LENGTH];
	int           request_header_count;
	char          request_content_type[HTTP_MAX_HEADER_VALUE_LENGTH];

	/* resp size including headers! */
	unsigned long max_response_size;
	int           response_header_count;
	char          response_content_type[HTTP_MAX_HEADER_VALUE_LENGTH];
	char          response_headers[HTTP_MAX_HEADER_COUNT][HTTP_MAX_HEADER_LENGTH];

	/* Equals the largest of either max_request_size or max_response_size */
	unsigned long max_request_or_response_size;

	/* Functions to initialize aspects of sandbox */
	mod_glb_fn_t  initialize_globals;
	mod_mem_fn_t  initialize_memory;
	mod_tbl_fn_t  initialize_tables;
	mod_libc_fn_t initialize_libc;

	/* Entry Function to invoke serverless function */
	mod_main_fn_t main;
};

/*************************
 * Public Static Inlines *
 ************************/

/**
 * Increment a modules reference count
 * @param module
 */
static inline void
module_acquire(struct module *module)
{
	module->reference_count++;
}

/**
 * Get a module's argument count
 * @param module
 * @returns the number of arguments
 */
static inline i32
module_get_argument_count(struct module *module)
{
	return module->argument_count;
}

/**
 * Invoke a module's initialize_globals
 * @param module
 */
static inline void
module_initialize_globals(struct module *module)
{
	/* called in a sandbox. */
	module->initialize_globals();
}

/**
 * Invoke a module's initialize_tables
 * @param module
 */
static inline void
module_initialize_table(struct module *module)
{
	/* called at module creation time (once only per module). */
	module->initialize_tables();
}

/**
 * Invoke a module's initialize_libc
 * @param module - module whose libc we are initializing
 * @param env - address?
 * @param arguments - address?
 */
static inline void
module_initialize_libc(struct module *module, i32 env, i32 arguments)
{
	/* called in a sandbox. */
	module->initialize_libc(env, arguments);
}

/**
 * Invoke a module's initialize_memory
 * @param module - the module whose memory we are initializing
 */
static inline void
module_initialize_memory(struct module *module)
{
	// called in a sandbox.
	module->initialize_memory();
}

/**
 * Validate module, defined as having a non-NULL dynamical library handle and entry function pointer
 * @param module - module to validate
 * @return true if valid. false if invalid
 */
static inline bool
module_is_valid(struct module *module)
{
	return (module && module->dynamic_library_handle && module->main);
}

/**
 * Invoke a module's entry function, forwarding on argc and argv
 * @param module
 * @param argc standard UNIX count of arguments
 * @param argv standard UNIX vector of arguments
 * @return return code of module's main function
 */
static inline i32
module_main(struct module *module, i32 argc, i32 argv)
{
	return module->main(argc, argv);
}

/**
 * Decrement a modules reference count
 * @param module
 */
static inline void
module_release(struct module *module)
{
	module->reference_count--;
}

/**
 * Sets the HTTP Request and Response Headers and Content type on a module
 * @param module
 * @param request_count
 * @param request_headers
 * @param request_content_type
 * @param response_count
 * @param response_headers
 * @param response_content_type
 */
static inline void
module_set_http_info(struct module *module, int request_count, char *request_headers, char request_content_type[],
                     int response_count, char *response_headers, char response_content_type[])
{
	assert(module);
	module->request_header_count = request_count;
	memcpy(module->request_headers, request_headers, HTTP_MAX_HEADER_LENGTH * HTTP_MAX_HEADER_COUNT);
	strcpy(module->request_content_type, request_content_type);
	module->response_header_count = response_count;
	memcpy(module->response_headers, response_headers, HTTP_MAX_HEADER_LENGTH * HTTP_MAX_HEADER_COUNT);
	strcpy(module->response_content_type, response_content_type);
}

/********************************
 * Public Methods from module.c *
 *******************************/

void           module_free(struct module *module);
struct module *module_new(char *mod_name, char *mod_path, i32 argument_count, u32 stack_sz, u32 max_heap,
                          u32 relative_deadline_us, int port, int req_sz, int resp_sz);
int            module_new_from_json(char *filename);

#endif /* SFRT_MODULE_H */
