/************************************************************//**
*
*	@file: memory.c
*	@author: Martin Fouilleul
*	@date: 24/10/2019
*	@revision:
*
*****************************************************************/
#include<stdlib.h> // malloc
#include<string.h> // memset
#include"memory.h"
#include"macro_helpers.h"

//--------------------------------------------------------------------------------
//NOTE(martin): default malloc base allocator
//--------------------------------------------------------------------------------
void* mem_base_reserve_malloc(void* context, u64 size)
{
	return(malloc(size));
}

void mem_base_release_malloc(void* context, void* ptr, u64 size)
{
	free(ptr);
}

void mem_base_nop(void* context, void* ptr, u64 size) {}

mem_base_allocator* mem_base_allocator_default()
{
	static mem_base_allocator base = {};
	if(base.reserve == 0)
	{
		base.reserve = mem_base_reserve_malloc;
		base.commit = mem_base_nop;
		base.decommit = mem_base_nop;
		base.release = mem_base_release_malloc;
	}
	return(&base);
}

//--------------------------------------------------------------------------------
//NOTE(martin): memory arena
//--------------------------------------------------------------------------------
void mem_arena_init(mem_arena* arena)
{
	mem_arena_options options = {};
	mem_arena_init_with_options(arena, &options);
}

void mem_arena_init_with_options(mem_arena* arena, mem_arena_options* options)
{
	arena->base = options->base ? options->base : mem_base_allocator_default();
	arena->cap = options->reserve ? options->reserve : MEM_ARENA_DEFAULT_RESERVE_SIZE;

	arena->ptr = (char*)mem_base_reserve(arena->base, arena->cap);
	arena->committed = 0;
	arena->offset = 0;
}

void mem_arena_release(mem_arena* arena)
{
	mem_base_release(arena->base, arena->ptr, arena->cap);
	memset(arena, 0, sizeof(mem_arena));
}

void* mem_arena_alloc(mem_arena* arena, u64 size)
{
	u64 nextOffset = arena->offset + size;
	ASSERT(nextOffset <= arena->cap);

	if(nextOffset > arena->committed)
	{
		u64 nextCommitted = AlignUpOnPow2(nextOffset, MEM_ARENA_COMMIT_ALIGNMENT);
		nextCommitted = ClampHighBound(nextCommitted, arena->cap);
		u64 commitSize = nextCommitted - arena->committed;
		mem_base_commit(arena->base, arena->ptr + arena->committed, commitSize);
		arena->committed = nextCommitted;
	}
	char* p = arena->ptr + arena->offset;
	arena->offset += size;

	return(p);
}

void mem_arena_clear(mem_arena* arena)
{
	arena->offset = 0;
}

//--------------------------------------------------------------------------------
//NOTE(martin): memory pool
//--------------------------------------------------------------------------------
void mem_pool_init(mem_pool* pool, u64 blockSize)
{
	mem_pool_options options = {};
	mem_pool_init_with_options(pool, blockSize, &options);
}
void mem_pool_init_with_options(mem_pool* pool, u64 blockSize, mem_pool_options* options)
{
	mem_arena_options arenaOptions = {.base = options->base, .reserve = options->reserve};
	mem_arena_init_with_options(&pool->arena, &arenaOptions);
	pool->blockSize = ClampLowBound(blockSize, sizeof(list_info));
	ListInit(&pool->freeList);
}

void mem_pool_release(mem_pool* pool)
{
	mem_arena_release(&pool->arena);
	memset(pool, 0, sizeof(mem_pool));
}

void* mem_pool_alloc_block(mem_pool* pool)
{
	if(ListEmpty(&pool->freeList))
	{
		return(mem_arena_alloc(&pool->arena, pool->blockSize));
	}
	else
	{
		return(ListPop(&pool->freeList));
	}
}

void mem_pool_release_block(mem_pool* pool, void* ptr)
{
	ASSERT((((char*)ptr) >= pool->arena.ptr) && (((char*)ptr) < (pool->arena.ptr + pool->arena.offset)));
	ListPush(&pool->freeList, (list_info*)ptr);
}

void mem_pool_clear(mem_pool* pool)
{
	mem_arena_clear(&pool->arena);
	ListInit(&pool->freeList);
}
