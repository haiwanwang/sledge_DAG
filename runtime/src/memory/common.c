#include "runtime.h"
#include "types.h"

/* Region initialization helper function */
EXPORT void
initialize_region(u32 offset, u32 data_count, char *data)
{
	assert(local_sandbox_context_cache.linear_memory_size >= data_count);
	assert(offset < local_sandbox_context_cache.linear_memory_size - data_count);

	/* FIXME: Hack around segmented and unsegmented access */
	memcpy(get_memory_ptr_for_runtime(offset, data_count), data, data_count);
}

void
add_function_to_table(u32 idx, u32 type_id, char *pointer)
{
	assert(idx < INDIRECT_TABLE_SIZE);
	assert(local_sandbox_context_cache.module_indirect_table != NULL);

	/* TODO: atomic for multiple concurrent invocations? */
	if (local_sandbox_context_cache.module_indirect_table[idx].type_id == type_id
	    && local_sandbox_context_cache.module_indirect_table[idx].func_pointer == pointer)
		return;

	local_sandbox_context_cache.module_indirect_table[idx] = (struct indirect_table_entry){
		.type_id = type_id, .func_pointer = pointer
	};
}

/* If we are using runtime globals, we need to populate them */
WEAK void
populate_globals()
{
	assert(0); /* FIXME: is this used in WASM as dynamic modules? */
}
