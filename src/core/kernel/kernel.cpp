#include <cassert>
#include "kernel.hpp"
#include "kernel_types.hpp"
#include "cpu.hpp"

Kernel::Kernel(CPU& cpu, Memory& mem, GPU& gpu)
	: cpu(cpu), regs(cpu.regs()), mem(mem), handleCounter(0), serviceManager(regs, mem, gpu, currentProcess, *this) {
	objects.reserve(512); // Make room for a few objects to avoid further memory allocs later
	portHandles.reserve(32);
	threadIndices.reserve(appResourceLimits.maxThreads);

	for (int i = 0; i < threads.size(); i++) {
		Thread& t = threads[i];

		t.index = i;
		t.tlsBase = VirtualAddrs::TLSBase + i * VirtualAddrs::TLSSize;
		t.status = ThreadStatus::Dead;
		t.waitList.clear();
		t.waitList.reserve(10); // Reserve some space for the wait list to avoid further memory allocs later
		// The state below isn't necessary to initialize but we do it anyways out of caution
		t.outPointer = 0;
		t.waitAll = false;
	}

	setVersion(1, 69);
}

void Kernel::serviceSVC(u32 svc) {
	switch (svc) {
		case 0x01: controlMemory(); break;
		case 0x02: queryMemory(); break;
		case 0x08: createThread(); break;
		case 0x09: exitThread(); break;
		case 0x0A: svcSleepThread(); break;
		case 0x0B: getThreadPriority(); break;
		case 0x0C: setThreadPriority(); break;
		case 0x13: svcCreateMutex(); break;
		case 0x14: svcReleaseMutex(); break;
		case 0x15: svcCreateSemaphore(); break;
		case 0x16: svcReleaseSemaphore(); break;
		case 0x17: svcCreateEvent(); break;
		case 0x18: svcSignalEvent(); break;
		case 0x19: svcClearEvent(); break;
		case 0x1E: createMemoryBlock(); break;
		case 0x1F: mapMemoryBlock(); break;
		case 0x21: createAddressArbiter(); break;
		case 0x22: arbitrateAddress(); break;
		case 0x23: svcCloseHandle(); break;
		case 0x24: waitSynchronization1(); break;
		case 0x25: waitSynchronizationN(); break;
		case 0x27: duplicateHandle(); break;
		case 0x28: getSystemTick(); break;
		case 0x2B: getProcessInfo(); break;
		case 0x2D: connectToPort(); break;
		case 0x32: sendSyncRequest(); break;
		case 0x35: getProcessID(); break;
		case 0x37: getThreadID(); break;
		case 0x38: getResourceLimit(); break;
		case 0x39: getResourceLimitLimitValues(); break;
		case 0x3A: getResourceLimitCurrentValues(); break;
		case 0x3D: outputDebugString(); break;
		default: Helpers::panic("Unimplemented svc: %X @ %08X", svc, regs[15]); break;
	}
}

void Kernel::setVersion(u8 major, u8 minor) {
	u16 descriptor = (u16(major) << 8) | u16(minor);

	kernelVersion = descriptor;
	mem.kernelVersion = descriptor; // The memory objects needs a copy because you can read the kernel ver from config mem
}

Handle Kernel::makeProcess(u32 id) {
	const Handle processHandle = makeObject(KernelObjectType::Process);
	const Handle resourceLimitHandle = makeObject(KernelObjectType::ResourceLimit);

	// Allocate data
	objects[processHandle].data = new Process(id);
	const auto processData = objects[processHandle].getData<Process>();

	// Link resource limit object with its parent process
	objects[resourceLimitHandle].data = &processData->limits;
	processData->limits.handle = resourceLimitHandle;
	return processHandle;
}

// Get a pointer to the process indicated by handle, taking into account that 0xFFFF8001 always refers to the current process
// Returns nullptr if the handle does not correspond to a process
KernelObject* Kernel::getProcessFromPID(Handle handle) {
	if (handle == KernelHandles::CurrentProcess) [[likely]] {
		return getObject(currentProcess, KernelObjectType::Process);
	} else {
		return getObject(handle, KernelObjectType::Process);
	}
}

void Kernel::deleteObjectData(KernelObject& object) {
	if (object.data == nullptr) {
		return;
	}

	// Resource limit and thread objects do not allocate heap data, so we don't delete anything

	switch (object.type) {
		case KernelObjectType::AddressArbiter: delete object.getData<AddressArbiter>(); return;
		case KernelObjectType::Archive: delete object.getData<ArchiveSession>(); return;
		case KernelObjectType::Directory: delete object.getData<DirectorySession>(); return;
		case KernelObjectType::Event: delete object.getData<Event>(); return;
		case KernelObjectType::File: delete object.getData<FileSession>(); return;
		case KernelObjectType::MemoryBlock: delete object.getData<MemoryBlock>(); return;
		case KernelObjectType::Port: delete object.getData<Port>(); return;
		case KernelObjectType::Process: delete object.getData<Process>(); return;
		case KernelObjectType::ResourceLimit: Helpers::panic("not known to allocate heap data"); return;
		case KernelObjectType::Session: delete object.getData<Session>(); return;
		case KernelObjectType::Mutex: delete object.getData<Mutex>(); return;
		case KernelObjectType::Semaphore: delete object.getData<Semaphore>(); return;
		case KernelObjectType::Thread: return;
		case KernelObjectType::Dummy: return;
		default: [[unlikely]] Helpers::warn("unknown object type"); return;
	}
}

void Kernel::reset() {
	handleCounter = 0;
	arbiterCount = 0;
	threadCount = 0;
	aliveThreadCount = 0;

	for (auto& t : threads) {
		t.status = ThreadStatus::Dead;
		t.waitList.clear();
		t.threadsWaitingForTermination = 0; // No threads are waiting for this thread to terminate cause it's dead
	}

	for (auto& object : objects) {
		deleteObjectData(object);
	}
	objects.clear();
	portHandles.clear();
	threadIndices.clear();
	serviceManager.reset();

	// Allocate handle #0 to a dummy object and make a main process object
	makeObject(KernelObjectType::Dummy);
	currentProcess = makeProcess(1); // Use ID = 1 for main process

	// Make main thread object. We do not have to set the entrypoint and SP for it as the ROM loader does.
	// Main thread seems to have a priority of 0x30. TODO: This creates a dummy context for thread 0,
	// which is thankfully not used. Maybe we should prevent this
	mainThread = makeThread(0, VirtualAddrs::StackTop, 0x30, -2, 0, ThreadStatus::Running);
	currentThreadIndex = 0;
	setupIdleThread();

	// Create some of the OS ports
	srvHandle = makePort("srv:"); // Service manager port
	errorPortHandle = makePort("err:f"); // Error display port
}

// Get pointer to thread-local storage
u32 Kernel::getTLSPointer() {
	return VirtualAddrs::TLSBase + currentThreadIndex * VirtualAddrs::TLSSize;
}

// Result CloseHandle(Handle handle)
void Kernel::svcCloseHandle() {
	logSVC("CloseHandle(handle = %d) (Unimplemented)\n", regs[0]);
	regs[0] = Result::Success;
}

// u64 GetSystemTick()
void Kernel::getSystemTick() {
	logSVC("GetSystemTick()\n");

	u64 ticks = cpu.getTicks();
	regs[0] = u32(ticks);
	regs[1] = u32(ticks >> 32);
}

// Result OutputDebugString(const char* str, s32 size)
// TODO: Does this actually write an error code in r0 and is the above signature correct?
void Kernel::outputDebugString() {
	const u32 pointer = regs[0];
	const u32 size = regs[1];

	std::string message = mem.readString(pointer, size);
	logDebugString("[OutputDebugString] %s\n", message.c_str());
	regs[0] = Result::Success;
}

void Kernel::getProcessID() {
	const auto pid = regs[1];
	const auto process = getProcessFromPID(pid);
	logSVC("GetProcessID(process: %s)\n", getProcessName(pid).c_str());

	if (process == nullptr) [[unlikely]] {
		regs[0] = Result::Kernel::InvalidHandle;
		return;
	}

	regs[0] = Result::Success;
	regs[1] = process->getData<Process>()->id;
}

// Result GetProcessInfo(s64* out, Handle process, ProcessInfoType type)
void Kernel::getProcessInfo() {
	const auto pid = regs[1];
	const auto type = regs[2];
	const auto process = getProcessFromPID(pid);
	logSVC("GetProcessInfo(process: %s, type = %d)\n", getProcessName(pid).c_str(), type);

	if (process == nullptr) [[unlikely]] {
		regs[0] = Result::Kernel::InvalidHandle;
		return;
	}

	switch (type) {
		// According to 3DBrew: Amount of private (code, data, heap) memory used by the process + total supervisor-mode
		// stack size + page-rounded size of the external handle table
		case 2:
			regs[1] = mem.getUsedUserMem();
			regs[2] = 0;
			break;

		case 20: // Returns 0x20000000 - <linear memory base vaddr for process>
			regs[1] = PhysicalAddrs::FCRAM - mem.getLinearHeapVaddr();
			regs[2] = 0;
			break;

		default:
			Helpers::panic("GetProcessInfo: unimplemented type %d", type);
	}

	regs[0] = Result::Success;
}

// Result DuplicateHandle(Handle* out, Handle original)
void Kernel::duplicateHandle() {
	Handle original = regs[1];
	logSVC("DuplicateHandle(handle = %X)\n", original);

	if (original == KernelHandles::CurrentThread) {
		regs[0] = Result::Success;
		Handle ret = makeObject(KernelObjectType::Thread);
		objects[ret].data = &threads[currentThreadIndex];

		regs[1] = ret;
	} else {
		Helpers::panic("DuplicateHandle: unimplemented handle type");
	}
}

std::string Kernel::getProcessName(u32 pid) {
	if (pid == KernelHandles::CurrentProcess) {
		return "current";
	} else {
		Helpers::panic("Attempted to name non-current process");
	}
}
