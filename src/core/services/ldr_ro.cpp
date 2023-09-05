#include "services/ldr_ro.hpp"
#include "ipc.hpp"

namespace LDRCommands {
	enum : u32 {
		Initialize = 0x000100C2,
		LoadCRR = 0x00020082,
		LoadCRONew = 0x000902C2,
	};
}

static constexpr u32 CRO_HEADER_SIZE = 0x138;

class CRO {
	// CRO header offsets
	static constexpr u32 HEADER_ID = 0x80;
	static constexpr u32 HEADER_NAME_OFFSET = 0x84;
	static constexpr u32 HEADER_NEXT_CRO = 0x88;
	static constexpr u32 HEADER_PREV_CRO = 0x8C;
	static constexpr u32 HEADER_CODE_OFFSET = 0xB0;
	static constexpr u32 HEADER_DATA_OFFSET = 0xB8;
	static constexpr u32 HEADER_MODULE_NAME_OFFSET = 0xC0;
	static constexpr u32 HEADER_SEGMENT_TABLE_OFFSET = 0xC8;
	static constexpr u32 HEADER_SEGMENT_TABLE_SIZE = 0xCC;
	static constexpr u32 HEADER_NAMED_EXPORT_TABLE_OFFSET = 0xD0;
	static constexpr u32 HEADER_NAMED_EXPORT_TABLE_SIZE = 0xD4;
	static constexpr u32 HEADER_INDEXED_EXPORT_TABLE_OFFSET = 0xD8;
	static constexpr u32 HEADER_EXPORT_STRINGS_OFFSET = 0xE0;
	static constexpr u32 HEADER_EXPORT_TREE_OFFSET = 0xE8;
	static constexpr u32 HEADER_IMPORT_MODULE_TABLE_OFFSET = 0xF0;
	static constexpr u32 HEADER_IMPORT_MODULE_TABLE_SIZE = 0xF4;
	static constexpr u32 HEADER_IMPORT_PATCHES_OFFSET = 0xF8;
	static constexpr u32 HEADER_NAMED_IMPORT_TABLE_OFFSET = 0x100;
	static constexpr u32 HEADER_NAMED_IMPORT_TABLE_SIZE = 0x104;
	static constexpr u32 HEADER_INDEXED_IMPORT_TABLE_OFFSET = 0x108;
	static constexpr u32 HEADER_INDEXED_IMPORT_TABLE_SIZE = 0x10C;
	static constexpr u32 HEADER_ANONYMOUS_IMPORT_TABLE_OFFSET = 0x110;
	static constexpr u32 HEADER_ANONYMOUS_IMPORT_TABLE_SIZE = 0x114;
	static constexpr u32 HEADER_IMPORT_STRINGS_OFFSET = 0x118;
	static constexpr u32 HEADER_STATIC_ANONYMOUS_SYMBOLS_OFFSET = 0x120;
	static constexpr u32 HEADER_RELOCATION_PATCHES_OFFSET = 0x128;
	static constexpr u32 HEADER_RELOCATION_PATCHES_SIZE = 0x12C;
	static constexpr u32 HEADER_STATIC_ANONYMOUS_PATCHES_OFFSET = 0x130;

	// Segment table entry offsets
	static constexpr u32 SEGMENT_OFFSET = 0;
	static constexpr u32 SEGMENT_ID = 8;
	static constexpr u32 SEGMENT_ENTRY_SIZE = 12;

	// Segment table entry IDs
	static constexpr u32 SEGMENT_ID_TEXT = 0;
	static constexpr u32 SEGMENT_ID_RODATA = 1;
	static constexpr u32 SEGMENT_ID_DATA = 2;
	static constexpr u32 SEGMENT_ID_BSS = 3;

	// Named export table
	static constexpr u32 NAMED_EXPORT_ENTRY_SIZE = 8;

	// Import module table
	static constexpr u32 IMPORT_MODULE_TABLE_NAME_OFFSET = 0;
	static constexpr u32 IMPORT_MODULE_TABLE_INDEXED_OFFSET = 8;
	static constexpr u32 IMPORT_MODULE_TABLE_ANONYMOUS_OFFSET = 16;
	static constexpr u32 IMPORT_MODULE_TABLE_ENTRY_SIZE = 20;

	// Named import table
	static constexpr u32 NAMED_IMPORT_NAME_OFFSET = 0;
	static constexpr u32 NAMED_IMPORT_RELOCATION_OFFSET = 4;
	static constexpr u32 NAMED_IMPORT_TABLE_ENTRY_SIZE = 8;

	// Indexed import table
	static constexpr u32 INDEXED_IMPORT_RELOCATION_OFFSET = 4;
	static constexpr u32 INDEXED_IMPORT_TABLE_ENTRY_SIZE = 8;

	// Anonymous import table
	static constexpr u32 ANONYMOUS_IMPORT_RELOCATION_OFFSET = 4;
	static constexpr u32 ANONYMOUS_IMPORT_TABLE_ENTRY_SIZE = 8;

	Memory &mem;

	u32 croPointer;

public:
	CRO(Memory &mem, u32 croPointer) : mem(mem), croPointer(croPointer) {}
	~CRO() = default;

	bool load() {
		const u8* header = (u8*)mem.getReadPointer(croPointer);

		// TODO: verify SHA hashes?

		// Verify CRO magic
		if (std::memcmp(&header[HEADER_ID], "CRO0", 4) != 0) {
			return false;
		}

		// These fields are initially 0, the RO service sets them on load. If non-0,
		// this CRO has already been loaded
		if ((*(u32*)&header[HEADER_NEXT_CRO] != 0) || (*(u32*)&header[HEADER_PREV_CRO] != 0)) {
			return false;
		}

		return true;
	}

	// Modify CRO offsets to point at virtual addresses
	bool rebase(u32 mapVaddr, u32 dataVaddr, u32 bssVaddr) {
		rebaseHeader(mapVaddr);

		u32 oldDataVaddr = 0;
		rebaseSegmentTable(mapVaddr, dataVaddr, bssVaddr, &oldDataVaddr);

		std::printf("Old .data vaddr = %X\n", oldDataVaddr);

		rebaseNamedExportTable(mapVaddr);
		rebaseImportModuleTable(mapVaddr);
		rebaseNamedImportTable(mapVaddr);
		rebaseIndexedImportTable(mapVaddr);
		rebaseAnonymousImportTable(mapVaddr);

		relocateInternal(oldDataVaddr);

		return true;
	}

	bool rebaseHeader(u32 mapVaddr) {
		std::puts("Rebasing CRO header");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		constexpr u32 headerOffsets[] = {
			HEADER_NAME_OFFSET,
			HEADER_CODE_OFFSET,
			HEADER_DATA_OFFSET,
			HEADER_MODULE_NAME_OFFSET,
			HEADER_SEGMENT_TABLE_OFFSET,
			HEADER_NAMED_EXPORT_TABLE_OFFSET,
			HEADER_INDEXED_EXPORT_TABLE_OFFSET,
			HEADER_EXPORT_STRINGS_OFFSET,
			HEADER_EXPORT_TREE_OFFSET,
			HEADER_IMPORT_MODULE_TABLE_OFFSET,
			HEADER_IMPORT_PATCHES_OFFSET,
			HEADER_NAMED_IMPORT_TABLE_OFFSET,
			HEADER_INDEXED_IMPORT_TABLE_OFFSET,
			HEADER_ANONYMOUS_IMPORT_TABLE_OFFSET,
			HEADER_IMPORT_STRINGS_OFFSET,
			HEADER_STATIC_ANONYMOUS_SYMBOLS_OFFSET, // ?
			HEADER_RELOCATION_PATCHES_OFFSET,
			HEADER_STATIC_ANONYMOUS_PATCHES_OFFSET, // ?
		};

		for (auto offset : headerOffsets) {
			*(u32*)&header[offset] += mapVaddr;
		}

		return true;
	}

	bool rebaseSegmentTable(u32 mapVaddr, u32 dataVaddr, u32 bssVaddr, u32 *oldDataVaddr) {
		std::puts("Rebasing segment table");

		const u8* header = (u8*)mem.getReadPointer(croPointer);
		
		const u32 segmentTableAddr = *(u32*)&header[HEADER_SEGMENT_TABLE_OFFSET];
		const u32 segmentTableSize = *(u32*)&header[HEADER_SEGMENT_TABLE_SIZE];

		if ((segmentTableAddr & 3) != 0) {
			Helpers::panic("Unaligned segment table address");
		}

		if (segmentTableSize == 0) {
			Helpers::panic("Segment table empty");
		}

		const u8* segmentTable = (u8*)mem.getReadPointer(segmentTableAddr);

		for (u32 segment = 0; segment < segmentTableSize; segment++) {
			u32* segmentOffset = (u32*)&segmentTable[SEGMENT_ENTRY_SIZE * segment + SEGMENT_OFFSET];

			const u32 segmentID = *(u32*)&segmentTable[SEGMENT_ENTRY_SIZE * segment + SEGMENT_ID];
			switch (segmentID) {
				case SEGMENT_ID_DATA:
					*oldDataVaddr = *segmentOffset + dataVaddr; *segmentOffset = dataVaddr; break;
				case SEGMENT_ID_BSS: *segmentOffset = bssVaddr; break;
				case SEGMENT_ID_TEXT:
				case SEGMENT_ID_RODATA:
					*segmentOffset += mapVaddr; break;
				default:
					Helpers::panic("Unknown segment ID");
			}

			std::printf("Rebasing segment table entry %u (ID = %u), addr = %X\n", segment, segmentID, *segmentOffset);
		}

		return true;
	}

	bool rebaseNamedExportTable(u32 mapVaddr) {
		std::puts("Rebasing named export table");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const u32 namedExportAddr = *(u32*)&header[HEADER_NAMED_EXPORT_TABLE_OFFSET];
		const u32 namedExportSize = *(u32*)&header[HEADER_NAMED_EXPORT_TABLE_SIZE];

		if ((namedExportAddr & 3) != 0) {
			Helpers::panic("Unaligned named export table address");
		}

		const u8* namedExportTable = (u8*)mem.getReadPointer(namedExportAddr);

		for (u32 namedExport = 0; namedExport < namedExportSize; namedExport++) {
			u32* nameOffset = (u32*)&namedExportTable[NAMED_EXPORT_ENTRY_SIZE * namedExport];

			assert(*nameOffset != 0);

			*nameOffset += mapVaddr;

			std::printf("Rebasing named export %u, addr = %X\n", namedExport, *nameOffset);
		}

		return true;
	}

	bool rebaseImportModuleTable(u32 mapVaddr) {
		std::puts("Rebasing import module table");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const u32 importModuleTableAddr = *(u32*)&header[HEADER_IMPORT_MODULE_TABLE_OFFSET];
		const u32 importModuleTableSize = *(u32*)&header[HEADER_IMPORT_MODULE_TABLE_SIZE];

		if ((importModuleTableAddr & 3) != 0) {
			Helpers::panic("Unaligned import module table address");
		}

		const u8* importModuleTable = (u8*)mem.getReadPointer(importModuleTableAddr);

		for (u32 importModule = 0; importModule < importModuleTableSize; importModule++) {
			u32* nameOffset = (u32*)&importModuleTable[IMPORT_MODULE_TABLE_ENTRY_SIZE * importModule + IMPORT_MODULE_TABLE_NAME_OFFSET];

			assert(*nameOffset != 0);

			*nameOffset += mapVaddr;

			u32 *indexedOffset = (u32*)&importModuleTable[IMPORT_MODULE_TABLE_ENTRY_SIZE * importModule + IMPORT_MODULE_TABLE_INDEXED_OFFSET];

			assert(*indexedOffset != 0);

			*indexedOffset += mapVaddr;

			u32 *anonymousOffset = (u32*)&importModuleTable[IMPORT_MODULE_TABLE_ENTRY_SIZE * importModule + IMPORT_MODULE_TABLE_ANONYMOUS_OFFSET];

			assert(*anonymousOffset != 0);

			*anonymousOffset += mapVaddr;

			std::printf("Rebasing import module %u, name addr = %X, indexed addr = %X, anonymous addr = %X\n", importModule, *nameOffset, *indexedOffset, *anonymousOffset);
		}

		return true;
	}

	bool rebaseNamedImportTable(u32 mapVaddr) {
		std::puts("Rebasing named import table");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const u32 namedImportTableAddr = *(u32*)&header[HEADER_NAMED_IMPORT_TABLE_OFFSET];
		const u32 namedImportTableSize = *(u32*)&header[HEADER_NAMED_IMPORT_TABLE_SIZE];

		if ((namedImportTableAddr & 3) != 0) {
			Helpers::panic("Unaligned named import table address");
		}

		const u8* namedImportTable = (u8*)mem.getReadPointer(namedImportTableAddr);

		for (u32 namedImport = 0; namedImport < namedImportTableSize; namedImport++) {
			u32* nameOffset = (u32*)&namedImportTable[NAMED_IMPORT_TABLE_ENTRY_SIZE * namedImport + NAMED_IMPORT_NAME_OFFSET];

			assert(*namedImport != 0);

			*nameOffset += mapVaddr;

			u32* relocationOffset = (u32*)&namedImportTable[NAMED_IMPORT_TABLE_ENTRY_SIZE * namedImport + NAMED_IMPORT_RELOCATION_OFFSET];

			assert(*relocationOffset != 0);

			*relocationOffset += mapVaddr;

			std::printf("Rebasing named import %u, name addr = %X, relocation addr = %X\n", namedImport, *nameOffset, *relocationOffset);
		}

		return true;
	}

	bool rebaseIndexedImportTable(u32 mapVaddr) {
		std::puts("Rebase indexed import table");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const u32 indexedImportTableAddr = *(u32*)&header[HEADER_INDEXED_IMPORT_TABLE_OFFSET];
		const u32 indexedImportTableSize = *(u32*)&header[HEADER_INDEXED_IMPORT_TABLE_SIZE];

		if ((indexedImportTableAddr & 3) != 0) {
			Helpers::panic("Unaligned indexed import table address");
		}

		const u8* indexedImportTable = (u8*)mem.getReadPointer(indexedImportTableAddr);

		for (u32 indexedImport = 0; indexedImport < indexedImportTableSize; indexedImport++) {
			u32* relocationOffset = (u32*)&indexedImportTable[INDEXED_IMPORT_TABLE_ENTRY_SIZE * indexedImport + INDEXED_IMPORT_RELOCATION_OFFSET];

			assert(*relocationOffset != 0);

			*relocationOffset += mapVaddr;

			std::printf("Rebasing indexed import %u, relocation addr = %X\n", indexedImport, *relocationOffset);
		}

		return true;
	}

	bool rebaseAnonymousImportTable(u32 mapVaddr) {
		std::puts("Rebase anonymous import table");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const u32 anonymousImportTableAddr = *(u32*)&header[HEADER_ANONYMOUS_IMPORT_TABLE_OFFSET];
		const u32 anonymousImportTableSize = *(u32*)&header[HEADER_ANONYMOUS_IMPORT_TABLE_SIZE];

		if ((anonymousImportTableAddr & 3) != 0) {
			Helpers::panic("Unaligned anonymous import table address");
		}

		const u8* anonymousImportTable = (u8*)mem.getReadPointer(anonymousImportTableAddr);

		for (u32 anonymousImport = 0; anonymousImport < anonymousImportTableSize; anonymousImport++) {
			u32* relocationOffset = (u32*)&anonymousImportTable[ANONYMOUS_IMPORT_TABLE_ENTRY_SIZE * anonymousImport + ANONYMOUS_IMPORT_RELOCATION_OFFSET];

			assert(*relocationOffset != 0);

			*relocationOffset += mapVaddr;

			std::printf("Rebasing anonymous import %u, relocation addr = %X\n", anonymousImport, *relocationOffset);
		}

		return true;
	}

	bool relocateInternal(u32 oldDataVaddr) {
		std::puts("Relocate internal");

		const u8* header = (u8*)mem.getReadPointer(croPointer);

		const u32 relocationTableAddr = *(u32*)&header[HEADER_RELOCATION_PATCHES_OFFSET];
		const u32 relocationTableSize = *(u32*)&header[HEADER_RELOCATION_PATCHES_SIZE];

		const u32 segmentTableAddr = *(u32*)&header[HEADER_SEGMENT_TABLE_OFFSET];
		//const u32 segmentTableSize = *(u32*)&header[HEADER_SEGMENT_TABLE_SIZE];

		const u8* relocationTable = (u8*)mem.getReadPointer(relocationTableAddr);
		const u8* segmentTable = (u8*)mem.getReadPointer(segmentTableAddr);

		for (u32 relocationNum = 0; relocationNum < relocationTableSize; relocationNum++) {
			const u32 segmentOffset = *(u32*)&relocationTable[12 * relocationNum];
			const u8 patchType = *(u8*)&relocationTable[12 * relocationNum + 4];
			const u8 index = *(u8*)&relocationTable[12 * relocationNum + 5];
			const u32 addend = *(u32*)&relocationTable[12 * relocationNum + 8];

			std::printf("Relocation %u, segment offset = %X, patch type = %X, index = %X, addend = %X\n", relocationNum, segmentOffset, patchType, index, addend);

			const u32 segmentAddr = getSegmentAddr(segmentOffset);

			// Get relocation target address
			const u32 entryID = *(u32*)&segmentTable[SEGMENT_ENTRY_SIZE * (segmentOffset & 0xF) + SEGMENT_ID];

			u32 relocationTarget = segmentAddr;
			if (entryID == SEGMENT_ID_DATA) {
				// Recompute relocation target for .data
				relocationTarget = oldDataVaddr + (segmentOffset >> 4);
			}

			if (relocationTarget == 0) {
				Helpers::panic("Relocation target is NULL");
			}

			const u32 symbolOffset = *(u32*)&segmentTable[SEGMENT_ENTRY_SIZE * index + SEGMENT_OFFSET];

			patchSymbol(relocationTarget, patchType, addend, symbolOffset);
		}

		return true;
	}

	bool patchSymbol(u32 relocationTarget, u8 patchType, u32 addend, u32 symbolOffset) {
		switch (patchType) {
			case 2: mem.write32(relocationTarget, symbolOffset + addend); break;
			default: Helpers::panic("Unhandled relocation type = %X\n", patchType);
		}

		return true;
	}

	u32 getSegmentAddr(u32 segmentOffset) {
		const u8* header = (u8*)mem.getReadPointer(croPointer);

		// "Decoded" segment tag
		const u32 segmentIndex = segmentOffset & 0xF;
		const u32 offset = segmentOffset >> 4;

		const u32 segmentTableAddr = *(u32*)&header[HEADER_SEGMENT_TABLE_OFFSET];
		const u32 segmentTableSize = *(u32*)&header[HEADER_SEGMENT_TABLE_SIZE];

		if (segmentIndex >= segmentTableSize) {
			Helpers::panic("bwaaa (invalid segment index = %u, table size = %u)", segmentIndex, segmentTableSize);
		}

		const u8* segmentTable = (u8*)mem.getReadPointer(segmentTableAddr);

		// Get segment table entry
		const u32 entryOffset = *(u32*)&segmentTable[SEGMENT_ENTRY_SIZE * segmentIndex];
		const u32 entrySize = *(u32*)&segmentTable[SEGMENT_ENTRY_SIZE * segmentIndex + 4];

		if (offset >= entrySize) {
			Helpers::panic("bwaaa (invalid offset = %X, entry size = %X)", offset, entrySize);
		}

		return entryOffset + offset;
	}
};

void LDRService::reset() {}

void LDRService::handleSyncRequest(u32 messagePointer) {
	const u32 command = mem.read32(messagePointer);
	switch (command) {
		case LDRCommands::Initialize: initialize(messagePointer); break;
		case LDRCommands::LoadCRR: loadCRR(messagePointer); break;
		case LDRCommands::LoadCRONew: loadCRONew(messagePointer); break;
		default: Helpers::panic("LDR::RO service requested. Command: %08X\n", command);
	}
}

void LDRService::initialize(u32 messagePointer) {
	const u32 crsPointer = mem.read32(messagePointer + 4);
	const u32 size = mem.read32(messagePointer + 8);
	const u32 mapVaddr = mem.read32(messagePointer + 12);
	const Handle process = mem.read32(messagePointer + 20);

	log("LDR_RO::Initialize (buffer = %08X, size = %08X, vaddr = %08X, process = %X)\n", crsPointer, size, mapVaddr, process);
	mem.write32(messagePointer, IPC::responseHeader(0x1, 1, 0));
	mem.write32(messagePointer + 4, Result::Success);
}

void LDRService::loadCRR(u32 messagePointer) {
	const u32 crrPointer = mem.read32(messagePointer + 4);
	const u32 size = mem.read32(messagePointer + 8);
	const Handle process = mem.read32(messagePointer + 20);

	log("LDR_RO::LoadCRR (buffer = %08X, size = %08X, process = %X)\n", crrPointer, size, process);
	mem.write32(messagePointer, IPC::responseHeader(0x2, 1, 0));
	mem.write32(messagePointer + 4, Result::Success);
}

void LDRService::loadCRONew(u32 messagePointer) {
	const u32 croPointer = mem.read32(messagePointer + 4);
	const u32 mapVaddr = mem.read32(messagePointer + 8);
	const u32 size = mem.read32(messagePointer + 12);
	const u32 dataVaddr = mem.read32(messagePointer + 16);
	const u32 dataSize = mem.read32(messagePointer + 24);
	const u32 bssVaddr = mem.read32(messagePointer + 28);
	const u32 bssSize = mem.read32(messagePointer + 32);
	const bool autoLink = mem.read32(messagePointer + 36) != 0;
	const u32 fixLevel = mem.read32(messagePointer + 40);
	const Handle process = mem.read32(messagePointer + 52);

	std::printf("LDR_RO::LoadCRONew (buffer = %08X, vaddr = %08X, size = %08X, .data vaddr = %08X, .data size = %08X, .bss vaddr = %08X, .bss size = %08X, auto link = %d, fix level = %X, process = %X)\n", croPointer, mapVaddr, size, dataVaddr, dataSize, bssVaddr, bssSize, autoLink, fixLevel, process);

	// Sanity checks
	if (size < CRO_HEADER_SIZE) {
		Helpers::panic("CRO too small\n");
	}

	if ((size & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRO size\n");
	}

	if ((croPointer & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRO pointer\n");
	}

	if ((mapVaddr & mem.pageMask) != 0) {
		Helpers::panic("Unaligned CRO output vaddr\n");
	}

	// Map CRO to output address
	mem.mirrorMapping(mapVaddr, croPointer, size);

	CRO cro(mem, croPointer);

	if (!cro.load()) {
		Helpers::panic("Failed to load CRO");
	}

	if (!cro.rebase(mapVaddr, dataVaddr, bssVaddr)) {
		Helpers::panic("Failed to rebase CRO");
	}

	mem.write32(messagePointer, IPC::responseHeader(0x9, 2, 0));
	mem.write32(messagePointer + 4, Result::Success);
	mem.write32(messagePointer + 8, size);
}