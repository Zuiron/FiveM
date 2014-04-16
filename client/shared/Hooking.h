#pragma once

#include <stdint.h>
#include <jitasm.h>

#include <memory>
#include <functional>

namespace hook
{
// for link /DYNAMICBASE executables
extern ptrdiff_t baseAddressDifference;

// sets the base address difference based on an obtained pointer
inline void set_base(uintptr_t address)
{
	uintptr_t addressDiff = (address - 0x400000);

	// pointer-style cast to ensure unsigned overflow ends up copied directly into a signed value
	baseAddressDifference = *(ptrdiff_t*)&addressDiff;
}

// sets the base to the process main base
inline void set_base()
{
	set_base((uintptr_t)GetModuleHandle(NULL));
}

// adjusts the address passed to the base as set above
inline void adjust_base(uintptr_t& address)
{
	address += baseAddressDifference;
}

// returns the adjusted address to the stated base
inline uintptr_t get_adjusted(uintptr_t address)
{
	return address + baseAddressDifference;
}

struct pass
{
	template<typename ...T> pass(T...) {}
};

#pragma region assembly generator
class FunctionAssembly
{
private:
	void* m_code;

public:
	inline FunctionAssembly(jitasm::Frontend& frontend)
	{
		frontend.Assemble();

		void* code;
		code = VirtualAlloc(0, frontend.GetCodeSize(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

		memcpy(code, frontend.GetCode(), frontend.GetCodeSize());

		m_code = code;
	}

	inline ~FunctionAssembly()
	{
		VirtualFree(m_code, 0, MEM_RELEASE);
	}

	inline void* GetCode()
	{
		return m_code;
	}
};
#pragma endregion

template<typename ValueType>
inline void put(uintptr_t address, ValueType value)
{
	adjust_base(address);

	memcpy((void*)address, &value, sizeof(value));
}

inline void nop(uintptr_t address, size_t length)
{
	adjust_base(address);

	memset((void*)address, 0x90, length);
}

inline void return_function(uintptr_t address, uint16_t stackSize = 0)
{
	if (stackSize == 0)
	{
		put<uint8_t>(address, 0xC3);
	}
	else
	{
		put<uint8_t>(address, 0xC2);
		put<uint16_t>(address + 1, stackSize);
	}
}

template<typename T>
inline T* getRVA(uintptr_t rva)
{
	return (T*)(baseAddressDifference + 0x400000 + rva);
}

template<typename T>
void iat(const char* moduleName, T function, int ordinal)
{
	IMAGE_DOS_HEADER* imageHeader = (IMAGE_DOS_HEADER*)(baseAddressDifference + 0x400000);
	IMAGE_NT_HEADERS* ntHeader = getRVA<IMAGE_NT_HEADERS>(imageHeader->e_lfanew);

	IMAGE_IMPORT_DESCRIPTOR* descriptor = getRVA<IMAGE_IMPORT_DESCRIPTOR>(ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	while (descriptor->Name)
	{
		const char* name = getRVA<char>(descriptor->Name);

		if (_stricmp(name, moduleName))
		{
			descriptor++;

			continue;
		}

		auto nameTableEntry = getRVA<uint32_t>(descriptor->OriginalFirstThunk);
		auto addressTableEntry = getRVA<uint32_t>(descriptor->FirstThunk);

		while (*nameTableEntry)
		{
			// is this an ordinal-only import?
			if (IMAGE_SNAP_BY_ORDINAL(*nameTableEntry))
			{
				if (IMAGE_ORDINAL(*nameTableEntry) == ordinal)
				{
					*addressTableEntry = (uint32_t)function;
					return;
				}
			}

			nameTableEntry++;
			addressTableEntry++;
		}

		return;
	}
}

// a context for the hook function to party on
struct HookContext
{
	uint32_t m_jumpRet;
	uint32_t m_edi, m_esi, m_ebp, m_esp, m_ebx, m_edx, m_ecx, m_eax; // matches pushad format
};

class inject_hook
{
friend struct inject_hook_frontend;

private:
	HookContext* m_curContext;
	std::shared_ptr<FunctionAssembly> m_assembly;
	uintptr_t m_address;

public:
	// return value type container
	typedef int ReturnType;

private:
	// set context and run
	ReturnType do_run(HookContext* context)
	{
		m_curContext = context;

		return run();
	}

protected:
	// return type values for use in return;

	// return without jumping anywhere
	inline ReturnType DoNowt()
	{
		return ReturnType(0);
	}

	// jump to this place after the hook
	inline ReturnType JumpTo(uint32_t address)
	{
		m_curContext->m_eax = address;

		return ReturnType(1);
	}

	// accessors for context registers
	inline uint32_t Eax() { return m_curContext->m_eax; }
	inline void Eax(uint32_t a) { m_curContext->m_eax = a; }

	inline uint32_t Ebx() { return m_curContext->m_ebx; }
	inline void Ebx(uint32_t a) { m_curContext->m_ebx = a; }

	inline uint32_t Ecx() { return m_curContext->m_ecx; }
	inline void Ecx(uint32_t a) { m_curContext->m_ecx = a; }

	inline uint32_t Edx() { return m_curContext->m_edx; }
	inline void Edx(uint32_t a) { m_curContext->m_edx = a; }

	inline uint32_t Esi() { return m_curContext->m_esi; }
	inline void Esi(uint32_t a) { m_curContext->m_esi = a; }

	inline uint32_t Edi() { return m_curContext->m_edi; }
	inline void Edi(uint32_t a) { m_curContext->m_edi = a; }

	inline uint32_t Esp() { return m_curContext->m_esp; }
	inline void Esp(uint32_t a) { m_curContext->m_esp = a; }

	inline uint32_t Ebp() { return m_curContext->m_ebp; }
	inline void Ebp(uint32_t a) { m_curContext->m_ebp = a; }

public:
	virtual ReturnType run() = 0;

	inject_hook(uint32_t address)
	{
		m_address = address;
	}

	void inject();
};

struct inject_hook_frontend : jitasm::Frontend
{
private:
	inject_hook* m_hook;

	static inject_hook::ReturnType CallHookFunction(inject_hook* hook, HookContext* context)
	{
		return hook->do_run(context);
	}

public:
	inject_hook_frontend(inject_hook* hook)
	{
		m_hook = hook;
	}

	void InternalMain()
	{
		// set up the context stack frame
		pushad(); // registers
		sub(esp, 4); // jump target area
		mov(dword_ptr[esp], 0);

		// load the context address into eax
		lea(eax, dword_ptr[esp]);

		// push eax (second argument to our call)
		push(eax);

		// push the (softcoded, heh) hook function
		push((uint32_t)m_hook);

		// call the call stub
		mov(eax, (uint32_t)CallHookFunction);
		call(eax);

		// remove garbage from the stack
		add(esp, 8);

		// do we want to jump somewhere?
		test(eax, eax);
		jnz("actuallyJump");

		// guess not, remove jump target area and popad
		add(esp, 4);
		popad();

		// get esp back from the context bit
		mov(esp, dword_ptr[esp - 20]);

		ret();

		L("actuallyJump");

		add(esp, 4);
		popad();

		mov(esp, dword_ptr[esp - 20]);
		
		AppendInstr(jitasm::I_CALL, 0xFF, 0, jitasm::Imm8(4), R(eax));
	}
};

#define DEFINE_INJECT_HOOK(hookName, hookAddress) class _zz_inject_hook_##hookName : public hook::inject_hook { public: _zz_inject_hook_##hookName(uint32_t address) : hook::inject_hook(address) {}; ReturnType run(); }; \
	static _zz_inject_hook_##hookName hookName(hookAddress); \
	_zz_inject_hook_##hookName::ReturnType _zz_inject_hook_##hookName::run()


#if 0
struct AsmHookStub : jitasm::Frontend
{
private:
	hook_function* m_safeFunction;

	static bool CallHookFunction(hook_function* function, HookContext& context)
	{
		return (*function)(context);
	}

public:
	AsmHookStub(hook_function* function)
	{
		m_safeFunction = function;
	}

	void InternalMain()
	{
		// set up the context stack frame
		pushad(); // registers
		sub(esp, 4); // jump target area
		mov(dword_ptr[esp], 0);

		// load the jump target into eax
		lea(eax, dword_ptr[esp]);

		// push eax (second argument to our call)
		push(eax);

		// push the (softcoded, heh) hook function
		push((uint32_t)m_safeFunction);

		// call the call stub
		mov(eax, (uint32_t)CallHookFunction);
		call(eax);

		// remove garbage from the stack
		add(esp, 8);

		// check if we want to execute the original trampoline
		test(al, al);
		jnz("noTrampolineReturn");



		L("noTrampolineReturn");

		
	}
};
#endif

/*inline void inject_hook(uintptr_t address, hook_function func)
{
	// allocate memory for the safe pointer (as the function is probably someplace on the stack)
	// TODO: find a way to register this memory for freeing on late shutdown
	hook_function* safe_ptr = new hook_function(func);


}*/

#pragma region inject call: call stub
template<typename R, typename... Args>
struct CallStub : jitasm::function<void, CallStub<R, Args...>>
{
private:
	void* m_target;

public:
	CallStub(void* target)
		: m_target(target)
	{
	}

	void main()
	{
		uint32_t stackOffset = 0;
		uint32_t argOffset = sizeof(uintptr_t) * 2; // as frame pointers are also kept here
		uint32_t argCleanup = 0;

		pass{([&]
		{
			int size = min(sizeof(Args), sizeof(uintptr_t));

			argOffset += size;
		}(), 1)...};

		// as this is the end, and the last argument isn't past the end
		argOffset -= 4;
		
		pass{([&]
		{
			mov(eax, dword_ptr[esp + stackOffset + argOffset]);
			push(eax);

			int size = max(sizeof(Args), sizeof(uintptr_t));

			stackOffset += size;
			argCleanup += size;
			argOffset -= size;
		}(), 1)...};

		mov(eax, (uintptr_t)m_target);
		call(eax);

		add(esp, argCleanup);
	}
};
#pragma endregion

#pragma region inject call
template<typename R, typename... Args>
class inject_call
{
private:
	R (*m_origAddress)(Args...);

	uintptr_t m_address;

	std::shared_ptr<FunctionAssembly> m_assembly;

public:
	inject_call(uintptr_t address)
	{
		if (*(uint8_t*)address != 0xE8)
		{
			throw std::exception("not a call");
		}

		m_address = address;
	}	

	void inject(R (*target)(Args...))
	{
		CallStub<R, Args...> stub(target);
		
		m_assembly = std::make_shared<FunctionAssembly>(stub);

		// store original
		int origAddress = *(int*)(m_address + 1);
		origAddress += 5;
		origAddress += m_address;

		m_origAddress = (R(*)(Args...))origAddress;

		// and patch
		put<int>(m_address + 1, (uintptr_t)m_assembly->GetCode() - (uintptr_t)get_adjusted(m_address) - 5);
	}

	R call()
	{
		return ((R(*)())m_origAddress)();
	}

	R call(Args... args)
	{
		return m_origAddress(args...);
	}
};
#pragma endregion
}