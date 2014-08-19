#include "stdafx.h"
#include "Utilities/Log.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/SysCalls/Modules.h"

#include "cellSync.h"

//void cellSync_init();
//Module cellSync("cellSync", cellSync_init);
Module *cellSync = nullptr;

#ifdef PRX_DEBUG
#include "rpcs3.h"
#include "prx_libsre.h"
u32 libsre;
u32 libsre_rtoc;

void fix_import(Module* module, u32 func, u32 addr)
{
	Memory.Write32((addr), 0x3d600000 | (func >> 16)); /* lis r11, (func_id >> 16) */
	Memory.Write32((addr) + 4, 0x616b0000 | (func & 0xffff)); /* ori r11, (func_id & 0xffff) */
	Memory.Write64((addr) + 8, 0x440000024e800020ull); /* sc + blr */

	module->Load(func);
}

#define FIX_IMPORT(module, func, addr) fix_import(module, getFunctionId(#func), addr)
	
#endif

s32 syncMutexInitialize(mem_ptr_t<CellSyncMutex> mutex)
{
	if (!mutex)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (mutex.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: set zero and sync
	mutex->m_data() = 0;
	InterlockedCompareExchange(&mutex->m_data(), 0, 0);
	return CELL_OK;
}

s32 cellSyncMutexInitialize(mem_ptr_t<CellSyncMutex> mutex)
{
	cellSync->Log("cellSyncMutexInitialize(mutex_addr=0x%x)", mutex.GetAddr());

	return syncMutexInitialize(mutex);
}

s32 cellSyncMutexLock(mem_ptr_t<CellSyncMutex> mutex)
{
	cellSync->Log("cellSyncMutexLock(mutex_addr=0x%x)", mutex.GetAddr());

	if (!mutex)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (mutex.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: increase u16 and remember its old value
	be_t<u16> old_order;
	while (true)
	{
		const u32 old_data = mutex->m_data();
		CellSyncMutex new_mutex;
		new_mutex.m_data() = old_data;

		old_order = new_mutex.m_order;
		new_mutex.m_order++; // increase m_order
		if (InterlockedCompareExchange(&mutex->m_data(), new_mutex.m_data(), old_data) == old_data) break;
	}

	// prx: wait until another u16 value == old value
	while (old_order != mutex->m_freed)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		if (Emu.IsStopped())
		{
			LOG_WARNING(HLE, "cellSyncMutexLock(mutex_addr=0x%x) aborted", mutex.GetAddr());
			break;
		}
	}

	// prx: sync
	InterlockedCompareExchange(&mutex->m_data(), 0, 0);
	return CELL_OK;
}

s32 cellSyncMutexTryLock(mem_ptr_t<CellSyncMutex> mutex)
{
	cellSync->Log("cellSyncMutexTryLock(mutex_addr=0x%x)", mutex.GetAddr());

	if (!mutex)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (mutex.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	while (true)
	{
		const u32 old_data = mutex->m_data();
		CellSyncMutex new_mutex;
		new_mutex.m_data() = old_data;

		// prx: compare two u16 values and exit if not equal
		if (new_mutex.m_order != new_mutex.m_freed)
		{
			return CELL_SYNC_ERROR_BUSY;
		}
		else
		{
			new_mutex.m_order++;
		}
		if (InterlockedCompareExchange(&mutex->m_data(), new_mutex.m_data(), old_data) == old_data) break;
	}

	return CELL_OK;
}

s32 cellSyncMutexUnlock(mem_ptr_t<CellSyncMutex> mutex)
{
	cellSync->Log("cellSyncMutexUnlock(mutex_addr=0x%x)", mutex.GetAddr());

	if (!mutex)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (mutex.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	InterlockedCompareExchange(&mutex->m_data(), 0, 0);

	while (true)
	{
		const u32 old_data = mutex->m_data();
		CellSyncMutex new_mutex;
		new_mutex.m_data() = old_data;

		new_mutex.m_freed++;
		if (InterlockedCompareExchange(&mutex->m_data(), new_mutex.m_data(), old_data) == old_data) break;
	}

	return CELL_OK;
}

s32 syncBarrierInitialize(mem_ptr_t<CellSyncBarrier> barrier, u16 total_count)
{
	if (!barrier)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (barrier.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}
	if (!total_count || total_count > 32767)
	{
		return CELL_SYNC_ERROR_INVAL;
	}

	// prx: zeroize first u16, write total_count in second u16 and sync
	barrier->m_value = 0;
	barrier->m_count = total_count;
	InterlockedCompareExchange(&barrier->m_data(), 0, 0);
	return CELL_OK;
}

s32 cellSyncBarrierInitialize(mem_ptr_t<CellSyncBarrier> barrier, u16 total_count)
{
	cellSync->Log("cellSyncBarrierInitialize(barrier_addr=0x%x, total_count=%d)", barrier.GetAddr(), total_count);

	return syncBarrierInitialize(barrier, total_count);
}

s32 cellSyncBarrierNotify(mem_ptr_t<CellSyncBarrier> barrier)
{
	cellSync->Log("cellSyncBarrierNotify(barrier_addr=0x%x)", barrier.GetAddr());

	if (!barrier)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (barrier.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: sync, extract m_value, repeat if < 0, increase, compare with second s16, set sign bit if equal, insert it back
	InterlockedCompareExchange(&barrier->m_data(), 0, 0);

	while (true)
	{
		const u32 old_data = barrier->m_data();
		CellSyncBarrier new_barrier;
		new_barrier.m_data() = old_data;

		s16 value = (s16)new_barrier.m_value;
		if (value < 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				LOG_WARNING(HLE, "cellSyncBarrierNotify(barrier_addr=0x%x) aborted", barrier.GetAddr());
				return CELL_OK;
			}
			continue;
		}

		value++;
		if (value == (s16)new_barrier.m_count)
		{
			value |= 0x8000;
		}
		new_barrier.m_value = value;
		if (InterlockedCompareExchange(&barrier->m_data(), new_barrier.m_data(), old_data) == old_data) break;
	}

	return CELL_OK;
}

s32 cellSyncBarrierTryNotify(mem_ptr_t<CellSyncBarrier> barrier)
{
	cellSync->Log("cellSyncBarrierTryNotify(barrier_addr=0x%x)", barrier.GetAddr());

	if (!barrier)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (barrier.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	InterlockedCompareExchange(&barrier->m_data(), 0, 0);

	while (true)
	{
		const u32 old_data = barrier->m_data();
		CellSyncBarrier new_barrier;
		new_barrier.m_data() = old_data;

		s16 value = (s16)new_barrier.m_value;
		if (value >= 0)
		{
			value++;
			if (value == (s16)new_barrier.m_count)
			{
				value |= 0x8000;
			}
			new_barrier.m_value = value;
			if (InterlockedCompareExchange(&barrier->m_data(), new_barrier.m_data(), old_data) == old_data) break;
		}		
		else
		{
			if (InterlockedCompareExchange(&barrier->m_data(), new_barrier.m_data(), old_data) == old_data) return CELL_SYNC_ERROR_BUSY;
		}
	}

	return CELL_OK;
}

s32 cellSyncBarrierWait(mem_ptr_t<CellSyncBarrier> barrier)
{
	cellSync->Log("cellSyncBarrierWait(barrier_addr=0x%x)", barrier.GetAddr());

	if (!barrier)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (barrier.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: sync, extract m_value (repeat if >= 0), decrease it, set 0 if == 0x8000, insert it back
	InterlockedCompareExchange(&barrier->m_data(), 0, 0);

	while (true)
	{
		const u32 old_data = barrier->m_data();
		CellSyncBarrier new_barrier;
		new_barrier.m_data() = old_data;

		s16 value = (s16)new_barrier.m_value;
		if (value >= 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				LOG_WARNING(HLE, "cellSyncBarrierWait(barrier_addr=0x%x) aborted", barrier.GetAddr());
				return CELL_OK;
			}
			continue;
		}

		value--;
		if (value == (s16)0x8000)
		{
			value = 0;
		}
		new_barrier.m_value = value;
		if (InterlockedCompareExchange(&barrier->m_data(), new_barrier.m_data(), old_data) == old_data) break;
	}

	return CELL_OK;
}

s32 cellSyncBarrierTryWait(mem_ptr_t<CellSyncBarrier> barrier)
{
	cellSync->Log("cellSyncBarrierTryWait(barrier_addr=0x%x)", barrier.GetAddr());

	if (!barrier)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (barrier.GetAddr() % 4)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	InterlockedCompareExchange(&barrier->m_data(), 0, 0);

	while (true)
	{
		const u32 old_data = barrier->m_data();
		CellSyncBarrier new_barrier;
		new_barrier.m_data() = old_data;

		s16 value = (s16)new_barrier.m_value;
		if (value >= 0)
		{
			return CELL_SYNC_ERROR_BUSY;
		}

		value--;
		if (value == (s16)0x8000)
		{
			value = 0;
		}
		new_barrier.m_value = value;
		if (InterlockedCompareExchange(&barrier->m_data(), new_barrier.m_data(), old_data) == old_data) break;
	}

	return CELL_OK;
}

s32 syncRwmInitialize(mem_ptr_t<CellSyncRwm> rwm, u32 buffer_addr, u32 buffer_size)
{
	if (!rwm || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (rwm.GetAddr() % 16 || buffer_addr % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}
	if (buffer_size % 128 || buffer_size > 0x4000)
	{
		return CELL_SYNC_ERROR_INVAL;
	}

	// prx: zeroize first u16 and second u16, write buffer_size in second u32, write buffer_addr in second u64 and sync
	rwm->m_data() = 0;
	rwm->m_size = buffer_size;
	rwm->m_addr = (u64)buffer_addr;
	InterlockedCompareExchange(&rwm->m_data(), 0, 0);
	return CELL_OK;
}

s32 cellSyncRwmInitialize(mem_ptr_t<CellSyncRwm> rwm, u32 buffer_addr, u32 buffer_size)
{
	cellSync->Log("cellSyncRwmInitialize(rwm_addr=0x%x, buffer_addr=0x%x, buffer_size=0x%x)", rwm.GetAddr(), buffer_addr, buffer_size);

	return syncRwmInitialize(rwm, buffer_addr, buffer_size);
}

s32 cellSyncRwmRead(mem_ptr_t<CellSyncRwm> rwm, u32 buffer_addr)
{
	cellSync->Log("cellSyncRwmRead(rwm_addr=0x%x, buffer_addr=0x%x)", rwm.GetAddr(), buffer_addr);

	if (!rwm || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (rwm.GetAddr() % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: atomically load first u32, repeat until second u16 == 0, increase first u16 and sync
	while (true)
	{
		const u32 old_data = rwm->m_data();
		CellSyncRwm new_rwm;
		new_rwm.m_data() = old_data;

		if (new_rwm.m_writers.ToBE())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncRwmRead(rwm_addr=0x%x) aborted", rwm.GetAddr());
				return CELL_OK;
			}
			continue;
		}
		
		new_rwm.m_readers++;
		if (InterlockedCompareExchange(&rwm->m_data(), new_rwm.m_data(), old_data) == old_data) break;
	}

	// copy data to buffer_addr
	memcpy(Memory + buffer_addr, Memory + (u64)rwm->m_addr, (u32)rwm->m_size);

	// prx: load first u32, return 0x8041010C if first u16 == 0, atomically decrease it
	while (true)
	{
		const u32 old_data = rwm->m_data();
		CellSyncRwm new_rwm;
		new_rwm.m_data() = old_data;

		if (!new_rwm.m_readers.ToBE())
		{
			cellSync->Error("cellSyncRwmRead(rwm_addr=0x%x): m_readers == 0 (m_writers=%d)", rwm.GetAddr(), (u16)new_rwm.m_writers);
			return CELL_SYNC_ERROR_ABORT;
		}

		new_rwm.m_readers--;
		if (InterlockedCompareExchange(&rwm->m_data(), new_rwm.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncRwmTryRead(mem_ptr_t<CellSyncRwm> rwm, u32 buffer_addr)
{
	cellSync->Log("cellSyncRwmTryRead(rwm_addr=0x%x, buffer_addr=0x%x)", rwm.GetAddr(), buffer_addr);

	if (!rwm || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (rwm.GetAddr() % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	while (true)
	{
		const u32 old_data = rwm->m_data();
		CellSyncRwm new_rwm;
		new_rwm.m_data() = old_data;

		if (new_rwm.m_writers.ToBE())
		{
			return CELL_SYNC_ERROR_BUSY;
		}

		new_rwm.m_readers++;
		if (InterlockedCompareExchange(&rwm->m_data(), new_rwm.m_data(), old_data) == old_data) break;
	}

	memcpy(Memory + buffer_addr, Memory + (u64)rwm->m_addr, (u32)rwm->m_size);

	while (true)
	{
		const u32 old_data = rwm->m_data();
		CellSyncRwm new_rwm;
		new_rwm.m_data() = old_data;

		if (!new_rwm.m_readers.ToBE())
		{
			cellSync->Error("cellSyncRwmRead(rwm_addr=0x%x): m_readers == 0 (m_writers=%d)", rwm.GetAddr(), (u16)new_rwm.m_writers);
			return CELL_SYNC_ERROR_ABORT;
		}

		new_rwm.m_readers--;
		if (InterlockedCompareExchange(&rwm->m_data(), new_rwm.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncRwmWrite(mem_ptr_t<CellSyncRwm> rwm, u32 buffer_addr)
{
	cellSync->Log("cellSyncRwmWrite(rwm_addr=0x%x, buffer_addr=0x%x)", rwm.GetAddr(), buffer_addr);

	if (!rwm || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (rwm.GetAddr() % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: atomically compare second u16 (m_writers) with 0, repeat if not 0, set 1, sync
	while (true)
	{
		const u32 old_data = rwm->m_data();
		CellSyncRwm new_rwm;
		new_rwm.m_data() = old_data;

		if (new_rwm.m_writers.ToBE())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncRwmWrite(rwm_addr=0x%x) aborted (I)", rwm.GetAddr());
				return CELL_OK;
			}
			continue;
		}

		new_rwm.m_writers = 1;
		if (InterlockedCompareExchange(&rwm->m_data(), new_rwm.m_data(), old_data) == old_data) break;
	}

	// prx: wait until m_readers == 0
	while (rwm->m_readers.ToBE())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		if (Emu.IsStopped())
		{
			cellSync->Warning("cellSyncRwmWrite(rwm_addr=0x%x) aborted (II)", rwm.GetAddr());
			return CELL_OK;
		}
	}

	// prx: copy data from buffer_addr
	memcpy(Memory + (u64)rwm->m_addr, Memory + buffer_addr, (u32)rwm->m_size);

	// prx: sync and zeroize m_readers and m_writers
	InterlockedCompareExchange(&rwm->m_data(), 0, 0);
	rwm->m_data() = 0;
	return CELL_OK;
}

s32 cellSyncRwmTryWrite(mem_ptr_t<CellSyncRwm> rwm, u32 buffer_addr)
{
	cellSync->Log("cellSyncRwmTryWrite(rwm_addr=0x%x, buffer_addr=0x%x)", rwm.GetAddr(), buffer_addr);

	if (!rwm || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (rwm.GetAddr() % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: compare m_readers | m_writers with 0, return busy if not zero, set m_writers to 1
	if (InterlockedCompareExchange(&rwm->m_data(), se32(1), 0) != 0) return CELL_SYNC_ERROR_BUSY;

	// prx: copy data from buffer_addr
	memcpy(Memory + (u64)rwm->m_addr, Memory + buffer_addr, (u32)rwm->m_size);

	// prx: sync and zeroize m_readers and m_writers
	InterlockedCompareExchange(&rwm->m_data(), 0, 0);
	rwm->m_data() = 0;
	return CELL_OK;
}

s32 syncQueueInitialize(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr, u32 size, u32 depth)
{
	if (!queue)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (size && !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32 || buffer_addr % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}
	if (!depth || size % 16)
	{
		return CELL_SYNC_ERROR_INVAL;
	}

	// prx: zeroize first u64, write size in third u32, write depth in fourth u32, write address in third u64 and sync
	queue->m_data() = 0;
	queue->m_size = size;
	queue->m_depth = depth;
	queue->m_addr = (u64)buffer_addr;
	InterlockedCompareExchange(&queue->m_data(), 0, 0);
	return CELL_OK;
}

s32 cellSyncQueueInitialize(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr, u32 size, u32 depth)
{
	cellSync->Log("cellSyncQueueInitialize(queue_addr=0x%x, buffer_addr=0x%x, size=0x%x, depth=0x%x)", queue.GetAddr(), buffer_addr, size, depth);

	return syncQueueInitialize(queue, buffer_addr, size, depth);
}

s32 cellSyncQueuePush(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr)
{
	cellSync->Log("cellSyncQueuePush(queue_addr=0x%x, buffer_addr=0x%x)", queue.GetAddr(), buffer_addr);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 size = (u32)queue->m_size;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);

	u32 position;
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		const u32 v2 = (u32)new_queue.m_v2;
		// prx: compare 5th u8 with zero (repeat if not zero)
		// prx: compare (second u32 (u24) + first u8) with depth (repeat if greater or equal)
		if ((v2 >> 24) || ((v2 & 0xffffff) + (v1 >> 24)) >= depth)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncQueuePush(queue_addr=0x%x) aborted", queue.GetAddr());
				return CELL_OK;
			}
			continue;
		}

		// prx: extract first u32 (u24) (-> position), calculate (position + 1) % depth, insert it back
		// prx: insert 1 in 5th u8
		// prx: extract second u32 (u24), increase it, insert it back
		position = (v1 & 0xffffff);
		new_queue.m_v1 = (v1 & 0xff000000) | ((position + 1) % depth);
		new_queue.m_v2 = (1 << 24) | ((v2 & 0xffffff) + 1);
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	// prx: memcpy(position * m_size + m_addr, buffer_addr, m_size), sync
	memcpy(Memory + ((u64)queue->m_addr + position * size), Memory + buffer_addr, size);

	// prx: atomically insert 0 in 5th u8
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		new_queue.m_v2 &= 0xffffff; // TODO: use InterlockedAnd() or something
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncQueueTryPush(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr)
{
	cellSync->Log("cellSyncQueueTryPush(queue_addr=0x%x, buffer_addr=0x%x)", queue.GetAddr(), buffer_addr);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 size = (u32)queue->m_size;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);

	u32 position;
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		const u32 v2 = (u32)new_queue.m_v2;
		if ((v2 >> 24) || ((v2 & 0xffffff) + (v1 >> 24)) >= depth)
		{
			return CELL_SYNC_ERROR_BUSY;
		}

		position = (v1 & 0xffffff);
		new_queue.m_v1 = (v1 & 0xff000000) | ((position + 1) % depth);
		new_queue.m_v2 = (1 << 24) | ((v2 & 0xffffff) + 1);
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	memcpy(Memory + ((u64)queue->m_addr + position * size), Memory + buffer_addr, size);

	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		new_queue.m_v2 &= 0xffffff; // TODO: use InterlockedAnd() or something
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncQueuePop(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr)
{
	cellSync->Log("cellSyncQueuePop(queue_addr=0x%x, buffer_addr=0x%x)", queue.GetAddr(), buffer_addr);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 size = (u32)queue->m_size;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);
	
	u32 position;
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		const u32 v2 = (u32)new_queue.m_v2;
		// prx: extract first u8, repeat if not zero
		// prx: extract second u32 (u24), subtract 5th u8, compare with zero, repeat if less or equal
		if ((v1 >> 24) || ((v2 & 0xffffff) <= (v2 >> 24)))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncQueuePop(queue_addr=0x%x) aborted", queue.GetAddr());
				return CELL_OK;
			}
			continue;
		}

		// prx: insert 1 in first u8
		// prx: extract first u32 (u24), add depth, subtract second u32 (u24), calculate (% depth), save to position
		// prx: extract second u32 (u24), decrease it, insert it back
		new_queue.m_v1 = 0x1000000 | v1;
		position = ((v1 & 0xffffff) + depth - (v2 & 0xffffff)) % depth;
		new_queue.m_v2 = (v2 & 0xff000000) | ((v2 & 0xffffff) - 1);
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	// prx: (sync), memcpy(buffer_addr, position * m_size + m_addr, m_size)
	memcpy(Memory + buffer_addr, Memory + ((u64)queue->m_addr + position * size), size);

	// prx: atomically insert 0 in first u8
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		new_queue.m_v1 &= 0xffffff; // TODO: use InterlockedAnd() or something
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncQueueTryPop(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr)
{
	cellSync->Log("cellSyncQueueTryPop(queue_addr=0x%x, buffer_addr=0x%x)", queue.GetAddr(), buffer_addr);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 size = (u32)queue->m_size;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);

	u32 position;
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		const u32 v2 = (u32)new_queue.m_v2;
		if ((v1 >> 24) || ((v2 & 0xffffff) <= (v2 >> 24)))
		{
			return CELL_SYNC_ERROR_BUSY;
		}

		new_queue.m_v1 = 0x1000000 | v1;
		position = ((v1 & 0xffffff) + depth - (v2 & 0xffffff)) % depth;
		new_queue.m_v2 = (v2 & 0xff000000) | ((v2 & 0xffffff) - 1);
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	memcpy(Memory + buffer_addr, Memory + ((u64)queue->m_addr + position * size), size);

	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		new_queue.m_v1 &= 0xffffff; // TODO: use InterlockedAnd() or something
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncQueuePeek(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr)
{
	cellSync->Log("cellSyncQueuePeek(queue_addr=0x%x, buffer_addr=0x%x)", queue.GetAddr(), buffer_addr);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 size = (u32)queue->m_size;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);

	u32 position;
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		const u32 v2 = (u32)new_queue.m_v2;
		if ((v1 >> 24) || ((v2 & 0xffffff) <= (v2 >> 24)))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncQueuePeek(queue_addr=0x%x) aborted", queue.GetAddr());
				return CELL_OK;
			}
			continue;
		}

		new_queue.m_v1 = 0x1000000 | v1;
		position = ((v1 & 0xffffff) + depth - (v2 & 0xffffff)) % depth;
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	memcpy(Memory + buffer_addr, Memory + ((u64)queue->m_addr + position * size), size);

	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		new_queue.m_v1 &= 0xffffff; // TODO: use InterlockedAnd() or something
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncQueueTryPeek(mem_ptr_t<CellSyncQueue> queue, u32 buffer_addr)
{
	cellSync->Log("cellSyncQueueTryPeek(queue_addr=0x%x, buffer_addr=0x%x)", queue.GetAddr(), buffer_addr);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 size = (u32)queue->m_size;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);

	u32 position;
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		const u32 v2 = (u32)new_queue.m_v2;
		if ((v1 >> 24) || ((v2 & 0xffffff) <= (v2 >> 24)))
		{
			return CELL_SYNC_ERROR_BUSY;
		}

		new_queue.m_v1 = 0x1000000 | v1;
		position = ((v1 & 0xffffff) + depth - (v2 & 0xffffff)) % depth;
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	memcpy(Memory + buffer_addr, Memory + ((u64)queue->m_addr + position * size), size);

	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		new_queue.m_v1 &= 0xffffff; // TODO: use InterlockedAnd() or something
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}
	return CELL_OK;
}

s32 cellSyncQueueSize(mem_ptr_t<CellSyncQueue> queue)
{
	cellSync->Log("cellSyncQueueSize(queue_addr=0x%x)", queue.GetAddr());

	if (!queue)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 count = (u32)queue->m_v2 & 0xffffff;
	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && count <= depth);

	return count;
}

s32 cellSyncQueueClear(mem_ptr_t<CellSyncQueue> queue)
{
	cellSync->Log("cellSyncQueueClear(queue_addr=0x%x)", queue.GetAddr());

	if (!queue)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 32)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	const u32 depth = (u32)queue->m_depth;
	assert(((u32)queue->m_v1 & 0xffffff) <= depth && ((u32)queue->m_v2 & 0xffffff) <= depth);

	// TODO: optimize if possible
	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v1 = (u32)new_queue.m_v1;
		// prx: extract first u8, repeat if not zero, insert 1
		if (v1 >> 24)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncQueueClear(queue_addr=0x%x) aborted (I)", queue.GetAddr());
				return CELL_OK;
			}
			continue;
		}
		new_queue.m_v1 = v1 | 0x1000000;
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	while (true)
	{
		const u64 old_data = queue->m_data();
		CellSyncQueue new_queue;
		new_queue.m_data() = old_data;

		const u32 v2 = (u32)new_queue.m_v2;
		// prx: extract 5th u8, repeat if not zero, insert 1
		if (v2 >> 24)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
			if (Emu.IsStopped())
			{
				cellSync->Warning("cellSyncQueueClear(queue_addr=0x%x) aborted (II)", queue.GetAddr());
				return CELL_OK;
			}
			continue;
		}
		new_queue.m_v2 = v2 | 0x1000000;
		if (InterlockedCompareExchange(&queue->m_data(), new_queue.m_data(), old_data) == old_data) break;
	}

	queue->m_data() = 0;
	InterlockedCompareExchange(&queue->m_data(), 0, 0);
	return CELL_OK;
}

// LFQueue functions

void syncLFQueueDump(mem_ptr_t<CellSyncLFQueue> queue)
{
	LOG_NOTICE(HLE, "CellSyncLFQueue dump: addr = 0x%x", queue.GetAddr());
	for (u32 i = 0; i < sizeof(CellSyncLFQueue) / 16; i++)
	{
		LOG_NOTICE(HLE, "*** 0x%.16llx 0x%.16llx", Memory.Read64(queue.GetAddr() + i * 16), Memory.Read64(queue.GetAddr() + i * 16 + 8));
	}
}

void syncLFQueueInit(mem_ptr_t<CellSyncLFQueue> queue, u32 buffer_addr, u32 size, u32 depth, CellSyncQueueDirection direction, u32 eaSignal_addr)
{
	queue->m_h1 = 0;
	queue->m_h2 = 0;
	queue->m_h4 = 0;
	queue->m_h5 = 0;
	queue->m_h6 = 0;
	queue->m_h8 = 0;
	queue->m_size = size;
	queue->m_depth = depth;
	queue->m_buffer = (u64)buffer_addr;
	queue->m_direction = direction;
	for (u32 i = 0; i < sizeof(queue->m_hs) / sizeof(queue->m_hs[0]); i++)
	{
		queue->m_hs[i] = 0;
	}
	queue->m_eaSignal = (u64)eaSignal_addr;

	if (direction == CELL_SYNC_QUEUE_ANY2ANY)
	{
		queue->m_h3 = 0;
		queue->m_h7 = 0;
		queue->m_buffer = (u64)buffer_addr | 1;
		queue->m_bs[0] = -1;
		queue->m_bs[1] = -1;
		//m_bs[2]
		//m_bs[3]
		queue->m_v1 = -1;
		queue->m_hs[0] = -1;
		queue->m_hs[16] = -1;
		queue->m_v2 = 0;
		queue->m_v3 = 0;
	}
	else
	{
		//m_h3
		//m_h7
		queue->m_bs[0] = -1; // written as u32
		queue->m_bs[1] = -1;
		queue->m_bs[2] = -1;
		queue->m_bs[3] = -1;
		queue->m_v1 = 0;
		queue->m_v2 = 0; // written as u64
		queue->m_v3 = 0;
	}
}

s32 syncLFQueueInitialize(mem_ptr_t<CellSyncLFQueue> queue, u32 buffer_addr, u32 size, u32 depth, CellSyncQueueDirection direction, u32 eaSignal_addr)
{
#ifdef PRX_DEBUG_XXX
	return GetCurrentPPUThread().FastCall(libsre + 0x205C, libsre_rtoc, queue.GetAddr(), buffer_addr, size, depth, direction, eaSignal_addr);
#else

	if (!queue)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (size)
	{
		if (!buffer_addr)
		{
			return CELL_SYNC_ERROR_NULL_POINTER;
		}
		if (size > 0x4000 || size % 16)
		{
			return CELL_SYNC_ERROR_INVAL;
		}
	}
	if (!depth || (depth >> 15) || direction > 3)
	{
		return CELL_SYNC_ERROR_INVAL;
	}
	if (queue.GetAddr() % 128 || buffer_addr % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// prx: get sdk version of current process, return non-zero result of sys_process_get_sdk_version
	s32 sdk_ver;
	s32 ret = process_get_sdk_version(process_getpid(), sdk_ver);
	if (ret != CELL_OK)
	{
		return ret;
	}
	if (sdk_ver == -1)
	{
		sdk_ver = 0x460000;
	}

	// prx: reserve u32 at 0x2c offset
	u32 old_value;
	while (true)
	{
		const u32 old_data = queue->m_data();
		CellSyncLFQueue new_data;
		new_data.m_data() = old_data;

		if (old_data)
		{
			if (sdk_ver > 0x17ffff && old_data != se32(2))
			{
				return CELL_SYNC_ERROR_STAT;
			}
			old_value = old_data;
		}
		else
		{
			if (sdk_ver > 0x17ffff)
			{
				for (u32 i = 0; i < sizeof(CellSyncLFQueue) / sizeof(u64); i++)
				{
					if ((u64&)Memory[queue.GetAddr() + i * sizeof(u64)])
					{
						return CELL_SYNC_ERROR_STAT;
					}
				}
			}
			new_data.m_data() = se32(1);
			old_value = se32(1);
		}

		if (InterlockedCompareExchange(&queue->m_data(), new_data.m_data(), old_data) == old_data) break;
	}

	if (old_value == se32(2))
	{
		if ((u32)queue->m_size != size || (u32)queue->m_depth != depth || (u64)queue->m_buffer != (u64)buffer_addr)
		{
			return CELL_SYNC_ERROR_INVAL;
		}
		if (sdk_ver > 0x17ffff)
		{
			if ((u64)queue->m_eaSignal != (u64)eaSignal_addr || (u32)queue->m_direction != direction)
			{
				return CELL_SYNC_ERROR_INVAL;
			}
		}
	}
	else
	{
		// prx: call internal function with same arguments
		syncLFQueueInit(queue, buffer_addr, size, depth, direction, eaSignal_addr);

		// prx: sync, zeroize u32 at 0x2c offset
		InterlockedCompareExchange(&queue->m_data(), 0, 0);
		queue->m_data() = 0;
	}

	// prx: sync
	InterlockedCompareExchange(&queue->m_data(), 0, 0);
	return CELL_OK;
#endif
}

s32 cellSyncLFQueueInitialize(mem_ptr_t<CellSyncLFQueue> queue, u32 buffer_addr, u32 size, u32 depth, CellSyncQueueDirection direction, u32 eaSignal_addr)
{
	cellSync->Warning("cellSyncLFQueueInitialize(queue_addr=0x%x, buffer_addr=0x%x, size=0x%x, depth=0x%x, direction=%d, eaSignal_addr=0x%x)",
		queue.GetAddr(), buffer_addr, size, depth, direction, eaSignal_addr);

	return syncLFQueueInitialize(queue, buffer_addr, size, depth, direction, eaSignal_addr);
}

s32 syncLFQueueGetPushPointer(mem_ptr_t<CellSyncLFQueue> queue, s32& pointer, u32 isBlocking, u32 useEventQueue)
{
	if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_PPU2SPU))
	{
		return CELL_SYNC_ERROR_PERM;
	}

	u32 var1 = 0;
	s32 depth = (u32)queue->m_depth;
	while (true)
	{
		while (true)
		{
			if (Emu.IsStopped())
			{
				return -1;
			}

			const u64 old_data = InterlockedCompareExchange(&queue->m_push1(), 0, 0);
			CellSyncLFQueue new_queue;
			new_queue.m_push1() = old_data;

			if (var1)
			{
				new_queue.m_h7 = 0;
			}
			if (isBlocking && useEventQueue && *(u32*)queue->m_bs == -1)
			{
				return CELL_SYNC_ERROR_STAT;
			}

			s32 var2 = (s32)(s16)new_queue.m_h8;
			s32 res;
			if (useEventQueue && ((s32)(u16)new_queue.m_h5 != var2 || new_queue.m_h7.ToBE() != 0))
			{
				res = CELL_SYNC_ERROR_BUSY;
			}
			else
			{
				var2 -= (s32)(u16)queue->m_h1;
				if (var2 < 0)
				{
					var2 += depth * 2;
				}

				if (var2 < depth)
				{
					pointer = (s16)new_queue.m_h8;
					if (pointer + 1 >= depth * 2)
					{
						new_queue.m_h8 = 0;
					}
					else
					{
						new_queue.m_h8++;
					}
					res = CELL_OK;
				}
				else if (!isBlocking)
				{
					res = CELL_SYNC_ERROR_AGAIN;
					if (!new_queue.m_h7.ToBE() || res)
					{
						return res;
					}
					break;
				}
				else if (!useEventQueue)
				{
					continue;
				}
				else
				{
					res = CELL_OK;
					new_queue.m_h7 = 3;
					if (isBlocking != 3)
					{
						break;
					}
				}
			}

			if (InterlockedCompareExchange(&queue->m_push1(), new_queue.m_push1(), old_data) == old_data)
			{
				if (!new_queue.m_h7.ToBE() || res)
				{
					return res;
				}
				break;
			}
		}

		u32 eq = (u32)queue->m_v3; // 0x7c
		sys_event_data event;
		assert(0);
		// TODO: sys_event_queue_receive (event data is not used), assert if error returned
		var1 = 1;
	}

	assert(0);
}

s32 _cellSyncLFQueueGetPushPointer(mem_ptr_t<CellSyncLFQueue> queue, mem32_t pointer, u32 isBlocking, u32 useEventQueue)
{
	cellSync->Todo("_cellSyncLFQueueGetPushPointer(queue_addr=0x%x, pointer_addr=0x%x, isBlocking=%d, useEventQueue=%d)",
		queue.GetAddr(), pointer.GetAddr(), isBlocking, useEventQueue);

	s32 pointer_value;
	s32 result = syncLFQueueGetPushPointer(queue, pointer_value, isBlocking, useEventQueue);
	pointer = pointer_value;
	return result;
}

s32 syncLFQueueGetPushPointer2(mem_ptr_t<CellSyncLFQueue> queue, s32& pointer, u32 isBlocking, u32 useEventQueue)
{
	// TODO
	//pointer = 0;
	assert(0);
	return CELL_OK;
}

s32 _cellSyncLFQueueGetPushPointer2(mem_ptr_t<CellSyncLFQueue> queue, mem32_t pointer, u32 isBlocking, u32 useEventQueue)
{
	// arguments copied from _cellSyncLFQueueGetPushPointer
	cellSync->Todo("_cellSyncLFQueueGetPushPointer2(queue_addr=0x%x, pointer_addr=0x%x, isBlocking=%d, useEventQueue=%d)",
		queue.GetAddr(), pointer.GetAddr(), isBlocking, useEventQueue);

	s32 pointer_value;
	s32 result = syncLFQueueGetPushPointer2(queue, pointer_value, isBlocking, useEventQueue);
	pointer = pointer_value;
	return result;
}

s32 syncLFQueueCompletePushPointer(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, const std::function<s32(u32 addr, u32 arg)> fpSendSignal)
{
	if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_PPU2SPU))
	{
		return CELL_SYNC_ERROR_PERM;
	}

	s32 depth = (u32)queue->m_depth;

	while (true)
	{
		const u32 old_data = InterlockedCompareExchange(&queue->m_push2(), 0, 0);
		CellSyncLFQueue new_queue;
		new_queue.m_push2() = old_data;

		const u32 old_data2 = queue->m_push3();
		new_queue.m_push3() = old_data2;

		s32 var1 = pointer - (u16)new_queue.m_h5;
		if (var1 < 0)
		{
			var1 += depth * 2;
		}

		s32 var2 = (s32)(s16)queue->m_h4 - (s32)(u16)queue->m_h1;
		if (var2 < 0)
		{
			var2 += depth * 2;
		}

		s32 var9_ = 15 - var1;
		// calculate (1 slw (15 - var1))
		if (var9_ & 0x30)
		{
			var9_ = 0;
		}
		else
		{
			var9_ = 1 << var9_;
		}
		s32 var9 = ~(var9_ | (u16)new_queue.m_h6);
		// count leading zeros in u16
		{
			u16 v = var9;
			for (var9 = 0; var9 < 16; var9++)
			{
				if (v & (1 << (15 - var9)))
				{
					break;
				}
			}
		}
		
		s32 var5 = (s32)(u16)new_queue.m_h6 | var9_;
		if (var9 & 0x30)
		{
			var5 = 0;
		}
		else
		{
			var5 <<= var9;
		}

		s32 var3 = (u16)new_queue.m_h5 + var9;
		if (var3 >= depth * 2)
		{
			var3 -= depth * 2;
		}

		u16 pack = new_queue.m_hs[0]; // three packed 5-bit fields

		s32 var4 = ((pack >> 10) & 0x1f) - ((pack >> 5) & 0x1f);
		if (var4 < 0)
		{
			var4 += 0x1e;
		}

		u32 var6;
		if (var2 + var4 <= 15 && ((pack >> 10) & 0x1f) != (pack & 0x1f))
		{
			s32 var8 = (pack & 0x1f) - ((pack >> 10) & 0x1f);
			if (var8 < 0)
			{
				var8 += 0x1e;
			}

			if (var9 > 1 && (u32)var8 > 1)
			{
				assert(16 - var2 <= 1);
			}

			s32 var11 = (pack >> 10) & 0x1f;
			if (var11 >= 15)
			{
				var11 -= 15;
			}

			u16 var12 = (pack >> 10) & 0x1f;
			if (var12 == 0x1d)
			{
				var12 = 0;
			}
			else
			{
				var12 = (var12 + 1) << 10;
			}

			new_queue.m_hs[0] = (pack & 0x83ff) | var12;
			var6 = (u16)queue->m_hs[1 + 2 * var11];
		}
		else
		{
			var6 = -1;
		}

		s32 var7 = (var3 << 16) | (var5 & 0xffff);

		if (InterlockedCompareExchange(&queue->m_push2(), new_queue.m_push2(), old_data) == old_data)
		{
			assert(var2 + var4 < 16);
			if (var6 != -1)
			{
				bool exch = InterlockedCompareExchange(&queue->m_push3(), re32(var7), old_data2) == old_data2;
				assert(exch);
				if (exch)
				{
					assert(fpSendSignal);
					return fpSendSignal((u64)queue->m_eaSignal, var6);
				}
			}
			else
			{
				pack = queue->m_hs[0];
				if ((pack & 0x1f) == ((pack >> 10) & 0x1f))
				{
					if (InterlockedCompareExchange(&queue->m_push3(), re32(var7), old_data2) == old_data2)
					{
						return CELL_OK;
					}
				}
			}
		}
	}

	assert(0);
}

s32 _cellSyncLFQueueCompletePushPointer(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, mem_func_ptr_t<s32(*)(u32 addr, u32 arg)> fpSendSignal)
{
	cellSync->Todo("_cellSyncLFQueueCompletePushPointer(queue_addr=0x%x, pointer=%d, fpSendSignal_addr=0x%x)",
		queue.GetAddr(), pointer, fpSendSignal.GetAddr());

	return syncLFQueueCompletePushPointer(queue, pointer, [fpSendSignal](u32 addr, u32 arg){ return fpSendSignal(addr, arg); });
}

s32 syncLFQueueCompletePushPointer2(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, const std::function<s32(u32 addr, u32 arg)> fpSendSignal)
{
	// TODO
	//if (fpSendSignal) return fpSendSignal(0, 0);
	assert(0);
	return CELL_OK;
}

s32 _cellSyncLFQueueCompletePushPointer2(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, mem_func_ptr_t<s32(*)(u32 addr, u32 arg)> fpSendSignal)
{
	// arguments copied from _cellSyncLFQueueCompletePushPointer
	cellSync->Todo("_cellSyncLFQueueCompletePushPointer2(queue_addr=0x%x, pointer=%d, fpSendSignal_addr=0x%x)",
		queue.GetAddr(), pointer, fpSendSignal.GetAddr());

	return syncLFQueueCompletePushPointer2(queue, pointer, [fpSendSignal](u32 addr, u32 arg){ return fpSendSignal(addr, arg); });
}

s32 _cellSyncLFQueuePushBody(mem_ptr_t<CellSyncLFQueue> queue, u32 buffer_addr, u32 isBlocking)
{
	// cellSyncLFQueuePush has 1 in isBlocking param, cellSyncLFQueueTryPush has 0
	cellSync->Warning("_cellSyncLFQueuePushBody(queue_addr=0x%x, buffer_addr=0x%x, isBlocking=%d)", queue.GetAddr(), buffer_addr, isBlocking);

	//return GetCurrentPPUThread().FastCall2(libsre + 0x1674, libsre_rtoc); // test

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128 || buffer_addr % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	s32 position;
	//syncLFQueueDump(queue);

#ifdef PRX_DEBUG
	MemoryAllocator<be_t<s32>> position_v;
#endif
	while (true)
	{
		s32 res;

		if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_ANY2ANY))
		{
#ifdef PRX_DEBUG_XXX
			res = GetCurrentPPUThread().FastCall(libsre + 0x24B0, libsre_rtoc, queue.GetAddr(), position_v.GetAddr(), isBlocking, 0);
			position = position_v->ToLE();
#else
			res = syncLFQueueGetPushPointer(queue, position, isBlocking, 0);
#endif
		}
		else
		{
#ifdef PRX_DEBUG
			res = GetCurrentPPUThread().FastCall(libsre + 0x3050, libsre_rtoc, queue.GetAddr(), position_v.GetAddr(), isBlocking, 0);
			position = position_v->ToLE();
#else
			res = syncLFQueueGetPushPointer2(queue, position, isBlocking, 0);
#endif
		}

		//LOG_NOTICE(HLE, "... position = %d", position);
		//syncLFQueueDump(queue);

		if (!isBlocking || res != CELL_SYNC_ERROR_AGAIN)
		{
			if (res)
			{
				return res;
			}
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		if (Emu.IsStopped())
		{
			cellSync->Warning("_cellSyncLFQueuePushBody(queue_addr=0x%x) aborted", queue.GetAddr());
			return CELL_OK;
		}
	}

	s32 depth = (u32)queue->m_depth;
	s32 size = (u32)queue->m_size;
	memcpy(Memory + (((u64)queue->m_buffer & ~1ull) + size * (position >= depth ? position - depth : position)), Memory + buffer_addr, size);

	s32 res;
	if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_ANY2ANY))
	{
#ifdef PRX_DEBUG_XXX
		res = GetCurrentPPUThread().FastCall(libsre + 0x26C0, libsre_rtoc, queue.GetAddr(), position, 0);
#else
		res = syncLFQueueCompletePushPointer(queue, position, nullptr);
#endif
	}
	else
	{
#ifdef PRX_DEBUG
		res = GetCurrentPPUThread().FastCall(libsre + 0x355C, libsre_rtoc, queue.GetAddr(), position, 0);
#else
		res = syncLFQueueCompletePushPointer2(queue, position, nullptr);
#endif
	}

	//syncLFQueueDump(queue);
	return res;
}

s32 syncLFQueueGetPopPointer(mem_ptr_t<CellSyncLFQueue> queue, s32& pointer, u32 isBlocking, u32, u32 useEventQueue)
{
	if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_SPU2PPU))
	{
		return CELL_SYNC_ERROR_PERM;
	}

	u32 var1 = 0;
	s32 depth = (u32)queue->m_depth;
	while (true)
	{
		while (true)
		{
			if (Emu.IsStopped())
			{
				return -1;
			}

			const u64 old_data = InterlockedCompareExchange(&queue->m_pop1(), 0, 0);
			CellSyncLFQueue new_queue;
			new_queue.m_pop1() = old_data;

			if (var1)
			{
				new_queue.m_h3 = 0;
			}
			if (isBlocking && useEventQueue && *(u32*)queue->m_bs == -1)
			{
				return CELL_SYNC_ERROR_STAT;
			}

			s32 var2 = (s32)(s16)new_queue.m_h4;
			s32 res;
			if (useEventQueue && ((s32)(u16)new_queue.m_h1 != var2 || new_queue.m_h3.ToBE() != 0))
			{
				res = CELL_SYNC_ERROR_BUSY;
			}
			else
			{
				var2 = (s32)(u16)queue->m_h5 - var2;
				if (var2 < 0)
				{
					var2 += depth * 2;
				}

				if (var2 > 0)
				{
					pointer = (s16)new_queue.m_h4;
					if (pointer + 1 >= depth * 2)
					{
						new_queue.m_h4 = 0;
					}
					else
					{
						new_queue.m_h4++;
					}
					res = CELL_OK;
				}
				else if (!isBlocking)
				{
					res = CELL_SYNC_ERROR_AGAIN;
					if (!new_queue.m_h3.ToBE() || res)
					{
						return res;
					}
					break;
				}
				else if (!useEventQueue)
				{
					continue;
				}
				else
				{
					res = CELL_OK;
					new_queue.m_h3 = 3;
					if (isBlocking != 3)
					{
						break;
					}
				}
			}

			if (InterlockedCompareExchange(&queue->m_pop1(), new_queue.m_pop1(), old_data) == old_data)
			{
				if (!new_queue.m_h3.ToBE() || res)
				{
					return res;
				}
				break;
			}
		}

		u32 eq = (u32)queue->m_v3; // 0x7c
		sys_event_data event;
		assert(0);
		// TODO: sys_event_queue_receive (event data is not used), assert if error returned
		var1 = 1;
	}

	assert(0);
}

s32 _cellSyncLFQueueGetPopPointer(mem_ptr_t<CellSyncLFQueue> queue, mem32_t pointer, u32 isBlocking, u32 arg4, u32 useEventQueue)
{
	cellSync->Todo("_cellSyncLFQueueGetPopPointer(queue_addr=0x%x, pointer_addr=0x%x, isBlocking=%d, arg4=%d, useEventQueue=%d)",
		queue.GetAddr(), pointer.GetAddr(), isBlocking, arg4, useEventQueue);

	s32 pointer_value;
	s32 result = syncLFQueueGetPopPointer(queue, pointer_value, isBlocking, arg4, useEventQueue);
	pointer = pointer_value;
	return result;
}

s32 syncLFQueueGetPopPointer2(mem_ptr_t<CellSyncLFQueue> queue, s32& pointer, u32 isBlocking, u32 useEventQueue)
{
	// TODO
	//pointer = 0;
	assert(0);
	return CELL_OK;
}

s32 _cellSyncLFQueueGetPopPointer2(mem_ptr_t<CellSyncLFQueue> queue, mem32_t pointer, u32 isBlocking, u32 useEventQueue)
{
	// arguments copied from _cellSyncLFQueueGetPopPointer
	cellSync->Todo("_cellSyncLFQueueGetPopPointer2(queue_addr=0x%x, pointer_addr=0x%x, isBlocking=%d, useEventQueue=%d)",
		queue.GetAddr(), pointer.GetAddr(), isBlocking, useEventQueue);

	s32 pointer_value;
	s32 result = syncLFQueueGetPopPointer2(queue, pointer_value, isBlocking, useEventQueue);
	pointer = pointer_value;
	return result;
}

s32 syncLFQueueCompletePopPointer(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, const std::function<s32(u32 addr, u32 arg)> fpSendSignal, u32 noQueueFull)
{
	if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_SPU2PPU))
	{
		return CELL_SYNC_ERROR_PERM;
	}

	s32 depth = (u32)queue->m_depth;

	while (true)
	{
		const u32 old_data = InterlockedCompareExchange(&queue->m_pop2(), 0, 0);
		CellSyncLFQueue new_queue;
		new_queue.m_pop2() = old_data;

		const u32 old_data2 = queue->m_pop3();
		new_queue.m_pop3() = old_data2;

		s32 var1 = pointer - (u16)new_queue.m_h1;
		if (var1 < 0)
		{
			var1 += depth * 2;
		}

		s32 var2 = (s32)(s16)queue->m_h8 - (s32)(u16)queue->m_h5;
		if (var2 < 0)
		{
			var2 += depth * 2;
		}

		s32 var9_ = 15 - var1;
		// calculate (1 slw (15 - var1))
		if (var9_ & 0x30)
		{
			var9_ = 0;
		}
		else
		{
			var9_ = 1 << var9_;
		}
		s32 var9 = ~(var9_ | (u16)new_queue.m_h2);
		// count leading zeros in u16
		{
			u16 v = var9;
			for (var9 = 0; var9 < 16; var9++)
			{
				if (v & (1 << (15 - var9)))
				{
					break;
				}
			}
		}

		s32 var5 = (s32)(u16)new_queue.m_h2 | var9_;
		if (var9 & 0x30)
		{
			var5 = 0;
		}
		else
		{
			var5 <<= var9;
		}

		s32 var3 = (u16)new_queue.m_h1 + var9;
		if (var3 >= depth * 2)
		{
			var3 -= depth * 2;
		}

		u16 pack = new_queue.m_hs[16]; // three packed 5-bit fields

		s32 var4 = ((pack >> 10) & 0x1f) - ((pack >> 5) & 0x1f);
		if (var4 < 0)
		{
			var4 += 0x1e;
		}

		u32 var6;
		if (noQueueFull || var2 + var4 > 15 || ((pack >> 10) & 0x1f) == (pack & 0x1f))
		{
			var6 = -1;
		}
		else
		{
			s32 var8 = (pack & 0x1f) - ((pack >> 10) & 0x1f);
			if (var8 < 0)
			{
				var8 += 0x1e;
			}

			if (var9 > 1 && (u32)var8 > 1)
			{
				assert(16 - var2 <= 1);
			}

			s32 var11 = (pack >> 10) & 0x1f;
			if (var11 >= 15)
			{
				var11 -= 15;
			}

			u16 var12 = (pack >> 10) & 0x1f;
			if (var12 == 0x1d)
			{
				var12 = 0;
			}
			else
			{
				var12 = (var12 + 1) << 10;
			}

			new_queue.m_hs[0] = (pack & 0x83ff) | var12;
			var6 = (u16)queue->m_hs[17 + 2 * var11];
		}

		s32 var7 = (var3 << 16) | (var5 & 0xffff);

		if (InterlockedCompareExchange(&queue->m_pop2(), new_queue.m_pop2(), old_data) == old_data)
		{
			if (var6 != -1)
			{
				bool exch = InterlockedCompareExchange(&queue->m_pop3(), re32(var7), old_data2) == old_data2;
				assert(exch);
				if (exch)
				{
					assert(fpSendSignal);
					return fpSendSignal((u64)queue->m_eaSignal, var6);
				}
			}
			else
			{
				pack = queue->m_hs[16];
				if ((pack & 0x1f) == ((pack >> 10) & 0x1f))
				{
					if (InterlockedCompareExchange(&queue->m_pop3(), re32(var7), old_data2) == old_data2)
					{
						return CELL_OK;
					}
				}
			}
		}
	}

	assert(0);
}

s32 _cellSyncLFQueueCompletePopPointer(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, mem_func_ptr_t<s32(*)(u32 addr, u32 arg)> fpSendSignal, u32 noQueueFull)
{
	// arguments copied from _cellSyncLFQueueCompletePushPointer + unknown argument (noQueueFull taken from LFQueue2CompletePopPointer)
	cellSync->Todo("_cellSyncLFQueueCompletePopPointer(queue_addr=0x%x, pointer=%d, fpSendSignal_addr=0x%x, noQueueFull=%d)",
		queue.GetAddr(), pointer, fpSendSignal.GetAddr(), noQueueFull);

	return syncLFQueueCompletePopPointer(queue, pointer, [fpSendSignal](u32 addr, u32 arg){ return fpSendSignal(addr, arg); }, noQueueFull);
}

s32 syncLFQueueCompletePopPointer2(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, const std::function<s32(u32 addr, u32 arg)> fpSendSignal, u32 noQueueFull)
{
	// TODO
	//if (fpSendSignal) fpSendSignal(0, 0);
	assert(0);
	return CELL_OK;
}

s32 _cellSyncLFQueueCompletePopPointer2(mem_ptr_t<CellSyncLFQueue> queue, s32 pointer, mem_func_ptr_t<s32(*)(u32 addr, u32 arg)> fpSendSignal, u32 noQueueFull)
{
	// arguments copied from _cellSyncLFQueueCompletePopPointer
	cellSync->Todo("_cellSyncLFQueueCompletePopPointer2(queue_addr=0x%x, pointer=%d, fpSendSignal_addr=0x%x, noQueueFull=%d)",
		queue.GetAddr(), pointer, fpSendSignal.GetAddr(), noQueueFull);

	return syncLFQueueCompletePopPointer2(queue, pointer, [fpSendSignal](u32 addr, u32 arg){ return fpSendSignal(addr, arg); }, noQueueFull);
}

s32 _cellSyncLFQueuePopBody(mem_ptr_t<CellSyncLFQueue> queue, u32 buffer_addr, u32 isBlocking)
{
	// cellSyncLFQueuePop has 1 in isBlocking param, cellSyncLFQueueTryPop has 0
	cellSync->Warning("_cellSyncLFQueuePopBody(queue_addr=0x%x, buffer_addr=0x%x, isBlocking=%d)", queue.GetAddr(), buffer_addr, isBlocking);

	if (!queue || !buffer_addr)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128 || buffer_addr % 16)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	s32 position;
#ifdef PRX_DEBUG
	MemoryAllocator<be_t<s32>> position_v;
#endif
	while (true)
	{
		s32 res;
		if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_ANY2ANY))
		{
#ifdef PRX_DEBUG_XXX
			res = GetCurrentPPUThread().FastCall(libsre + 0x2A90, libsre_rtoc, queue.GetAddr(), position_v.GetAddr(), isBlocking, 0, 0);
			position = position_v->ToLE();
#else
			res = syncLFQueueGetPopPointer(queue, position, isBlocking, 0, 0);
#endif
		}
		else
		{
#ifdef PRX_DEBUG
			res = GetCurrentPPUThread().FastCall(libsre + 0x39AC, libsre_rtoc, queue.GetAddr(), position_v.GetAddr(), isBlocking, 0);
			position = position_v->ToLE();
#else
			res = syncLFQueueGetPopPointer2(queue, position, isBlocking, 0);
#endif
		}

		if (!isBlocking || res != CELL_SYNC_ERROR_AGAIN)
		{
			if (res)
			{
				return res;
			}
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack
		if (Emu.IsStopped())
		{
			cellSync->Warning("_cellSyncLFQueuePopBody(queue_addr=0x%x) aborted", queue.GetAddr());
			return CELL_OK;
		}
	}

	s32 depth = (u32)queue->m_depth;
	s32 size = (u32)queue->m_size;
	memcpy(Memory + buffer_addr, Memory + (((u64)queue->m_buffer & ~1ull) + size * (position >= depth ? position - depth : position)), size);

	s32 res;
	if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_ANY2ANY))
	{
#ifdef PRX_DEBUG_XXX
		res = GetCurrentPPUThread().FastCall(libsre + 0x2CA8, libsre_rtoc, queue.GetAddr(), position, 0, 0);
#else
		res = syncLFQueueCompletePopPointer(queue, position, nullptr, 0);
#endif
	}
	else
	{
#ifdef PRX_DEBUG
		res = GetCurrentPPUThread().FastCall(libsre + 0x3EB8, libsre_rtoc, queue.GetAddr(), position, 0, 0);
#else
		res = syncLFQueueCompletePopPointer2(queue, position, nullptr, 0);
#endif
	}

	return res;
}

s32 cellSyncLFQueueClear(mem_ptr_t<CellSyncLFQueue> queue)
{
	cellSync->Warning("cellSyncLFQueueClear(queue_addr=0x%x)", queue.GetAddr());

	if (!queue)
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// TODO: optimize if possible
	while (true)
	{
		const u64 old_data = InterlockedCompareExchange(&queue->m_pop1(), 0, 0);
		CellSyncLFQueue new_queue;
		new_queue.m_pop1() = old_data;

		const u64 new_data = queue->m_push1();
		new_queue.m_push1() = new_data;

		s32 var1, var2;
		if (queue->m_direction.ToBE() != se32(CELL_SYNC_QUEUE_ANY2ANY))
		{
			var1 = var2 = (u16)queue->m_hs[16];
		}
		else
		{
			var1 = (u16)new_queue.m_h7;
			var2 = (u16)new_queue.m_h3;
		}

		if ((s32)(s16)new_queue.m_h4 != (s32)(u16)new_queue.m_h1 ||
			(s32)(s16)new_queue.m_h8 != (s32)(u16)new_queue.m_h5 ||
			((var2 >> 10) & 0x1f) != (var2 & 0x1f) ||
			((var1 >> 10) & 0x1f) != (var1 & 0x1f))
		{
			return CELL_SYNC_ERROR_BUSY;
		}

		if (InterlockedCompareExchange(&queue->m_pop1(), new_data, old_data) == old_data) break;
	}

	return CELL_OK;
}

s32 cellSyncLFQueueSize(mem_ptr_t<CellSyncLFQueue> queue, mem32_t size)
{
	cellSync->Warning("cellSyncLFQueueSize(queue_addr=0x%x, size_addr=0x%x)", queue.GetAddr(), size.GetAddr());

	if (!queue || !size.GetAddr())
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	// TODO: optimize if possible
	while (true)
	{
		const u32 old_data = InterlockedCompareExchange(&queue->m_pop3(), 0, 0);

		u32 var1 = (u16)queue->m_h1;
		u32 var2 = (u16)queue->m_h5;

		if (InterlockedCompareExchange(&queue->m_pop3(), old_data, old_data) == old_data)
		{
			if (var1 <= var2)
			{
				size = var2 - var1;
			}
			else
			{
				size = var2 - var1 + (u32)queue->m_depth * 2;
			}
			return CELL_OK;
		}
	}

	assert(0);
}

s32 cellSyncLFQueueDepth(mem_ptr_t<CellSyncLFQueue> queue, mem32_t depth)
{
	cellSync->Log("cellSyncLFQueueDepth(queue_addr=0x%x, depth_addr=0x%x)", queue.GetAddr(), depth.GetAddr());

	if (!queue || !depth.GetAddr())
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	depth = queue->m_depth;
	return CELL_OK;
}

s32 _cellSyncLFQueueGetSignalAddress(mem_ptr_t<CellSyncLFQueue> queue, mem32_t ppSignal)
{
	cellSync->Log("_cellSyncLFQueueGetSignalAddress(queue_addr=0x%x, ppSignal_addr=0x%x)", queue.GetAddr(), ppSignal.GetAddr());

	if (!queue || !ppSignal.GetAddr())
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	ppSignal = queue->m_eaSignal;
	return CELL_OK;
}

s32 cellSyncLFQueueGetDirection(mem_ptr_t<CellSyncLFQueue> queue, mem32_t direction)
{
	cellSync->Log("cellSyncLFQueueGetDirection(queue_addr=0x%x, direction_addr=0x%x)", queue.GetAddr(), direction.GetAddr());

	if (!queue || !direction.GetAddr())
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	direction = queue->m_direction;
	return CELL_OK;
}

s32 cellSyncLFQueueGetEntrySize(mem_ptr_t<CellSyncLFQueue> queue, mem32_t entry_size)
{
	cellSync->Log("cellSyncLFQueueGetEntrySize(queue_addr=0x%x, entry_size_addr=0x%x)", queue.GetAddr(), entry_size.GetAddr());

	if (!queue || !entry_size.GetAddr())
	{
		return CELL_SYNC_ERROR_NULL_POINTER;
	}
	if (queue.GetAddr() % 128)
	{
		return CELL_SYNC_ERROR_ALIGN;
	}

	entry_size = queue->m_size;
	return CELL_OK;
}

s32 syncLFQueueAttachLv2EventQueue(mem32_ptr_t spus, u32 num, mem_ptr_t<CellSyncLFQueue> queue)
{
	// TODO
	assert(0);
	return CELL_OK;
}

s32 _cellSyncLFQueueAttachLv2EventQueue(mem32_ptr_t spus, u32 num, mem_ptr_t<CellSyncLFQueue> queue)
{
	cellSync->Todo("_cellSyncLFQueueAttachLv2EventQueue(spus_addr=0x%x, num=%d, queue_addr=0x%x)", spus.GetAddr(), num, queue.GetAddr());

	return syncLFQueueAttachLv2EventQueue(spus, num, queue);
}

s32 syncLFQueueDetachLv2EventQueue(mem32_ptr_t spus, u32 num, mem_ptr_t<CellSyncLFQueue> queue)
{
	// TODO
	assert(0);
	return CELL_OK;
}

s32 _cellSyncLFQueueDetachLv2EventQueue(mem32_ptr_t spus, u32 num, mem_ptr_t<CellSyncLFQueue> queue)
{
	cellSync->Todo("_cellSyncLFQueueDetachLv2EventQueue(spus_addr=0x%x, num=%d, queue_addr=0x%x)", spus.GetAddr(), num, queue.GetAddr());

	return syncLFQueueDetachLv2EventQueue(spus, num, queue);
}

void cellSync_init()
{
	cellSync->AddFunc(0xa9072dee, cellSyncMutexInitialize);
	cellSync->AddFunc(0x1bb675c2, cellSyncMutexLock);
	cellSync->AddFunc(0xd06918c4, cellSyncMutexTryLock);
	cellSync->AddFunc(0x91f2b7b0, cellSyncMutexUnlock);

	cellSync->AddFunc(0x07254fda, cellSyncBarrierInitialize);
	cellSync->AddFunc(0xf06a6415, cellSyncBarrierNotify);
	cellSync->AddFunc(0x268edd6d, cellSyncBarrierTryNotify);
	cellSync->AddFunc(0x35f21355, cellSyncBarrierWait);
	cellSync->AddFunc(0x6c272124, cellSyncBarrierTryWait);

	cellSync->AddFunc(0xfc48b03f, cellSyncRwmInitialize);
	cellSync->AddFunc(0xcece771f, cellSyncRwmRead);
	cellSync->AddFunc(0xa6669751, cellSyncRwmTryRead);
	cellSync->AddFunc(0xed773f5f, cellSyncRwmWrite);
	cellSync->AddFunc(0xba5bee48, cellSyncRwmTryWrite);

	cellSync->AddFunc(0x3929948d, cellSyncQueueInitialize);
	cellSync->AddFunc(0x5ae841e5, cellSyncQueuePush);
	cellSync->AddFunc(0x705985cd, cellSyncQueueTryPush);
	cellSync->AddFunc(0x4da6d7e0, cellSyncQueuePop);
	cellSync->AddFunc(0xa58df87f, cellSyncQueueTryPop);
	cellSync->AddFunc(0x48154c9b, cellSyncQueuePeek);
	cellSync->AddFunc(0x68af923c, cellSyncQueueTryPeek);
	cellSync->AddFunc(0x4da349b2, cellSyncQueueSize);
	cellSync->AddFunc(0xa5362e73, cellSyncQueueClear);

	cellSync->AddFunc(0x0c7cb9f7, cellSyncLFQueueGetEntrySize);
	cellSync->AddFunc(0x167ea63e, cellSyncLFQueueSize);
	cellSync->AddFunc(0x2af0c515, cellSyncLFQueueClear);
	cellSync->AddFunc(0x35bbdad2, _cellSyncLFQueueCompletePushPointer2);
	cellSync->AddFunc(0x46356fe0, _cellSyncLFQueueGetPopPointer2);
	cellSync->AddFunc(0x4e88c68d, _cellSyncLFQueueCompletePushPointer);
	cellSync->AddFunc(0x54fc2032, _cellSyncLFQueueAttachLv2EventQueue);
	cellSync->AddFunc(0x6bb4ef9d, _cellSyncLFQueueGetPushPointer2);
	cellSync->AddFunc(0x74c37666, _cellSyncLFQueueGetPopPointer);
	cellSync->AddFunc(0x7a51deee, _cellSyncLFQueueCompletePopPointer2);
	cellSync->AddFunc(0x811d148e, _cellSyncLFQueueDetachLv2EventQueue);
	cellSync->AddFunc(0xaa355278, cellSyncLFQueueInitialize);
	cellSync->AddFunc(0xaff7627a, _cellSyncLFQueueGetSignalAddress);
	cellSync->AddFunc(0xba5961ca, _cellSyncLFQueuePushBody);
	cellSync->AddFunc(0xd59aa307, cellSyncLFQueueGetDirection);
	cellSync->AddFunc(0xe18c273c, cellSyncLFQueueDepth);
	cellSync->AddFunc(0xe1bc7add, _cellSyncLFQueuePopBody);
	cellSync->AddFunc(0xe9bf2110, _cellSyncLFQueueGetPushPointer);
	cellSync->AddFunc(0xfe74e8e7, _cellSyncLFQueueCompletePopPointer);

#ifdef PRX_DEBUG
	wxGetApp().CallAfter([&]()
	{
		libsre = Memory.PRXMem.AllocAlign(sizeof(libsre_data), 4096);
		memcpy(Memory + libsre, libsre_data, sizeof(libsre_data));
		libsre_rtoc = libsre + 0x399B0;

		extern Module* sysPrxForUser;

		FIX_IMPORT(sysPrxForUser, _sys_strncmp                  , libsre + 0x1D5FC);
		FIX_IMPORT(sysPrxForUser, _sys_strcat                   , libsre + 0x1D61C);
		FIX_IMPORT(sysPrxForUser, _sys_vsnprintf                , libsre + 0x1D63C);
		FIX_IMPORT(sysPrxForUser, _sys_snprintf                 , libsre + 0x1D65C);
		FIX_IMPORT(sysPrxForUser, sys_lwmutex_lock              , libsre + 0x1D67C);
		FIX_IMPORT(sysPrxForUser, sys_lwmutex_unlock            , libsre + 0x1D69C);
		FIX_IMPORT(sysPrxForUser, sys_lwcond_destroy            , libsre + 0x1D6BC);
		FIX_IMPORT(sysPrxForUser, sys_ppu_thread_create         , libsre + 0x1D6DC);
		FIX_IMPORT(sysPrxForUser, sys_lwcond_wait               , libsre + 0x1D6FC);
		FIX_IMPORT(sysPrxForUser, _sys_strlen                   , libsre + 0x1D71C);
		FIX_IMPORT(sysPrxForUser, sys_lwmutex_create            , libsre + 0x1D73C);
		FIX_IMPORT(sysPrxForUser, _sys_spu_printf_detach_group  , libsre + 0x1D75C);
		FIX_IMPORT(sysPrxForUser, _sys_memset                   , libsre + 0x1D77C);
		FIX_IMPORT(sysPrxForUser, _sys_memcpy                   , libsre + 0x1D79C);
		FIX_IMPORT(sysPrxForUser, _sys_strncat                  , libsre + 0x1D7BC);
		FIX_IMPORT(sysPrxForUser, _sys_strcpy                   , libsre + 0x1D7DC);
		FIX_IMPORT(sysPrxForUser, _sys_printf                   , libsre + 0x1D7FC);
		fix_import(sysPrxForUser, 0x9FB6228E                    , libsre + 0x1D81C);
		FIX_IMPORT(sysPrxForUser, sys_ppu_thread_exit           , libsre + 0x1D83C);
		FIX_IMPORT(sysPrxForUser, sys_lwmutex_destroy           , libsre + 0x1D85C);
		FIX_IMPORT(sysPrxForUser, _sys_strncpy                  , libsre + 0x1D87C);
		FIX_IMPORT(sysPrxForUser, sys_lwcond_create             , libsre + 0x1D89C);
		FIX_IMPORT(sysPrxForUser, _sys_spu_printf_attach_group  , libsre + 0x1D8BC);
		FIX_IMPORT(sysPrxForUser, sys_prx_get_module_id_by_name , libsre + 0x1D8DC);
		FIX_IMPORT(sysPrxForUser, sys_spu_image_close           , libsre + 0x1D8FC);
		fix_import(sysPrxForUser, 0xE75C40F2                    , libsre + 0x1D91C);
		FIX_IMPORT(sysPrxForUser, sys_spu_image_import          , libsre + 0x1D93C);
		FIX_IMPORT(sysPrxForUser, sys_lwcond_signal             , libsre + 0x1D95C);
		FIX_IMPORT(sysPrxForUser, _sys_vprintf                  , libsre + 0x1D97C);
		FIX_IMPORT(sysPrxForUser, _sys_memcmp                   , libsre + 0x1D99C);
	});
#endif
}

#undef PRX_DEBUG
#undef FIX_IMPORT