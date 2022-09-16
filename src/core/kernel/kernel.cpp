#include <cassert>
#include "kernel.hpp"
#include "kernel_types.hpp"

void Kernel::serviceSVC(u32 svc) {
	switch (svc) {
		case 0x01: controlMemory(); break;
		case 0x21: createAddressArbiter(); break;
		case 0x23: svcCloseHandle(); break;
		case 0x2D: connectToPort(); break;
		case 0x38: getResourceLimit(); break;
		case 0x39: getResourceLimitLimitValues(); break;
		case 0x3A: getResourceLimitCurrentValues(); break;
		default: Helpers::panic("Unimplemented svc: %X @ %08X", svc, regs[15]); break;
	}
}

Handle Kernel::makeProcess() {
	const Handle processHandle = makeObject(KernelObjectType::Process);
	const Handle resourceLimitHandle = makeObject(KernelObjectType::ResourceLimit);

	// Allocate data
	objects[processHandle].data = new ProcessData();
	const auto processData = static_cast<ProcessData*>(objects[processHandle].data);

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

void Kernel::reset() {
	handleCounter = 0;

	for (auto& object : objects) {
		if (object.data != nullptr) {
			delete object.data;
		}
	}
	objects.clear();

	// Make a main process object
	currentProcess = makeProcess();
}

// Result CreateAddressArbiter(Handle* arbiter)
void Kernel::createAddressArbiter() {
	printf("Stubbed call to CreateAddressArbiter. Handle address: %08X\n", regs[0]);
	regs[0] = SVCResult::Success;
}

// Result CloseHandle(Handle handle)
void Kernel::svcCloseHandle() {
	printf("CloseHandle(handle = %d) (Unimplemented)\n", regs[0]);
	regs[0] = SVCResult::Success;
}

void Kernel::connectToPort() {
	const u32 handlePointer = regs[0];
	const char* port = static_cast<const char*>(mem.getReadPointer(regs[1]));

	printf("ConnectToPort(handle pointer = %08X, port = \"%s\")\n", handlePointer, port);
	Helpers::panic("Unimplemented IPC");
}

std::string Kernel::getProcessName(u32 pid) {
	if (pid == KernelHandles::CurrentProcess) {
		return "current";
	} else {
		Helpers::panic("Attempted to name non-current process");
	}
}