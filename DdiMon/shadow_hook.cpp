// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.

/// @file
/// Implements shadow hook functions.

#include "shadow_hook.h"

_IRQL_requires_max_(PASSIVE_LEVEL) static std::unique_ptr<HookInformation> ShpCreateHookInformation(_In_ SharedShadowHookData* shared_sh_data, _In_ void* address, _In_ ShadowHookTarget* target);
_IRQL_requires_max_(PASSIVE_LEVEL) _Success_(return ) EXTERN_C static bool ShpSetupInlineHook(_In_ void* patch_address, _In_ UCHAR* shadow_exec_page, _Out_ void** original_call_ptr);
_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C static SIZE_T ShpGetInstructionSize(_In_ void* address);
_IRQL_requires_max_(PASSIVE_LEVEL) EXTERN_C static TrampolineCode ShpMakeTrampolineCode(_In_ void* hook_handler);
static HookInformation* ShpFindPatchInfoByPage(_In_ const SharedShadowHookData* shared_sh_data, _In_ void* address);
static HookInformation* ShpFindPatchInfoByAddress(_In_ const SharedShadowHookData* shared_sh_data, _In_ void* address);
static void ShpEnablePageShadowingForExec(_In_ const HookInformation& info, _In_ EptData* ept_data);
static void ShpEnablePageShadowingForRW(_In_ const HookInformation& info, _In_ EptData* ept_data);
static void ShpDisablePageShadowing(_In_ const HookInformation& info, _In_ EptData* ept_data);
static void ShpSetMonitorTrapFlag(_In_ ShadowHookData* sh_data, _In_ bool enable);
static void ShpSaveLastHookInfo(_In_ ShadowHookData* sh_data, _In_ const HookInformation& info);
static const HookInformation* ShpRestoreLastHookInfo(_In_ ShadowHookData* sh_data);
static bool ShpIsShadowHookActive(_In_ const SharedShadowHookData* shared_sh_data);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, ShAllocateShadowHookData)
#pragma alloc_text(INIT, ShAllocateSharedShaowHookData)
#pragma alloc_text(INIT, ShEnableHooks)
#pragma alloc_text(INIT, ShInstallHook)
#pragma alloc_text(INIT, ShpSetupInlineHook)
#pragma alloc_text(INIT, ShpGetInstructionSize)
#pragma alloc_text(INIT, ShpMakeTrampolineCode)
#pragma alloc_text(PAGE, ShFreeShadowHookData)
#pragma alloc_text(PAGE, ShFreeSharedShadowHookData)
#pragma alloc_text(PAGE, ShDisableHooks)
#endif


_Use_decl_annotations_ EXTERN_C ShadowHookData* ShAllocateShadowHookData()// Allocates per-processor shadow hook data
{
    PAGED_CODE();
    auto p = new ShadowHookData();
    RtlFillMemory(p, sizeof(ShadowHookData), 0);
    return p;
}


_Use_decl_annotations_ EXTERN_C void ShFreeShadowHookData(ShadowHookData* sh_data)// Frees per-processor shadow hook data
{
    PAGED_CODE();
    delete sh_data;
}


_Use_decl_annotations_ EXTERN_C SharedShadowHookData* ShAllocateSharedShaowHookData()// Allocates processor-shared shadow hook data
{
    PAGED_CODE();
    auto p = new SharedShadowHookData();
    RtlFillMemory(p, sizeof(SharedShadowHookData), 0);
    return p;
}


_Use_decl_annotations_ EXTERN_C void ShFreeSharedShadowHookData(SharedShadowHookData* shared_sh_data)// Frees processor-shared shadow hook data
{
    PAGED_CODE();
    delete shared_sh_data;
}


_Use_decl_annotations_ EXTERN_C NTSTATUS ShEnableHooks()// Enables page shadowing for all hooks
{
    PAGED_CODE();
    return UtilForEachProcessor([](void* context){UNREFERENCED_PARAMETER(context); return UtilVmCall(HypercallNumber::kShEnablePageShadowing, nullptr);}, nullptr);
}


_Use_decl_annotations_ EXTERN_C NTSTATUS ShDisableHooks()// Disables page shadowing for all hooks
{
    PAGED_CODE();
    return UtilForEachProcessor([](void* context) {UNREFERENCED_PARAMETER(context); return UtilVmCall(HypercallNumber::kShDisablePageShadowing, nullptr); }, nullptr);
}


_Use_decl_annotations_ void ShEnablePageShadowing(EptData* ept_data, const SharedShadowHookData* shared_sh_data)// Enables page shadowing for all hooks
{
    for (auto& info : shared_sh_data->hooks)
    {
        ShpEnablePageShadowingForExec(*info, ept_data);
    }
}


_Use_decl_annotations_ void ShVmCallDisablePageShadowing(EptData* ept_data, const SharedShadowHookData* shared_sh_data)// Disables page shadowing for all hooks
{
    for (auto& info : shared_sh_data->hooks)
    {
        ShpDisablePageShadowing(*info, ept_data);
    }
}


_Use_decl_annotations_ bool ShHandleBreakpoint(ShadowHookData* sh_data, const SharedShadowHookData* shared_sh_data, void* guest_ip)
// Handles #BP. Checks if the #BP happened on where DdiMon set a break point, and if so, modifies the contents of guest's IP to execute a corresponding hook handler.
{
    UNREFERENCED_PARAMETER(sh_data);

    if (!ShpIsShadowHookActive(shared_sh_data)) {
        return false;
    }

    const auto info = ShpFindPatchInfoByAddress(shared_sh_data, guest_ip);
    if (!info) {
        return false;
    }

    UtilVmWrite(VmcsField::kGuestRip, reinterpret_cast<ULONG_PTR>(info->handler));// Update guest's IP
    return true;
}


_Use_decl_annotations_ void ShHandleMonitorTrapFlag(ShadowHookData* sh_data, const SharedShadowHookData* shared_sh_data, EptData* ept_data)
// Handles MTF VM-exit. Re-enables the shadow hook and clears MTF.
{
    NT_VERIFY(ShpIsShadowHookActive(shared_sh_data));
    const auto info = ShpRestoreLastHookInfo(sh_data);
    ShpEnablePageShadowingForExec(*info, ept_data);
    ShpSetMonitorTrapFlag(sh_data, false);
}


_Use_decl_annotations_ void ShHandleEptViolation(ShadowHookData* sh_data, const SharedShadowHookData* shared_sh_data, EptData* ept_data, void* fault_va)
// Handles EPT violation VM-exit.
{
    if (!ShpIsShadowHookActive(shared_sh_data)) {
        return;
    }

    const auto info = ShpFindPatchInfoByPage(shared_sh_data, fault_va);
    if (!info) {
        return;
    }

    // EPT violation was caused because a guest tried to read or write to a page where currently set as execute only for protecting a hook.
    // Let a guest read or write a page from a read/write shadow page and run a single instruction.
    ShpEnablePageShadowingForRW(*info, ept_data);
    ShpSetMonitorTrapFlag(sh_data, true);
    ShpSaveLastHookInfo(sh_data, *info);
}


_Use_decl_annotations_ EXTERN_C bool ShInstallHook(SharedShadowHookData* shared_sh_data, void* address, ShadowHookTarget* target)
// Set up inline hook at the address without activating it
{
    PAGED_CODE();

    auto info = ShpCreateHookInformation(shared_sh_data, reinterpret_cast<void*>(address), target);
    if (!info) {
        return false;
    }

    if (!ShpSetupInlineHook(info->patch_address, info->shadow_page_base_for_exec->page, &target->original_call)) {
        return false;
    }

    HYPERPLATFORM_LOG_DEBUG("Patch = %p, Exec = %p, RW = %p, Trampoline = %p",
        info->patch_address, info->shadow_page_base_for_exec->page + BYTE_OFFSET(info->patch_address), info->shadow_page_base_for_rw->page + BYTE_OFFSET(info->patch_address), target->original_call);

    shared_sh_data->hooks.push_back(std::move(info));
    return true;
}


_Use_decl_annotations_ static std::unique_ptr<HookInformation> ShpCreateHookInformation(SharedShadowHookData* shared_sh_data, void* address, ShadowHookTarget* target)
// Creates or reuses a couple of copied pages and initializes HookInformation
{
    auto info = std::make_unique<HookInformation>();
    auto reusable_info = ShpFindPatchInfoByPage(shared_sh_data, address);
    if (reusable_info) {// Found an existing HookInformation object targeting the same page as this one. re-use shadow pages.
        info->shadow_page_base_for_rw = reusable_info->shadow_page_base_for_rw;
        info->shadow_page_base_for_exec = reusable_info->shadow_page_base_for_exec;
    } else {// This hook is for a page that is not currently have any hooks (i.e., not shadowed). Creates shadow pages.
        info->shadow_page_base_for_rw = std::make_shared<Page>();
        info->shadow_page_base_for_exec = std::make_shared<Page>();
        auto page_base = PAGE_ALIGN(address);
        RtlCopyMemory(info->shadow_page_base_for_rw->page, page_base, PAGE_SIZE);
        RtlCopyMemory(info->shadow_page_base_for_exec->page, page_base, PAGE_SIZE);
    }
    info->patch_address = address;
    info->pa_base_for_rw = UtilPaFromVa(info->shadow_page_base_for_rw->page);
    info->pa_base_for_exec = UtilPaFromVa(info->shadow_page_base_for_exec->page);
    info->handler = target->handler;
    return info;
}


_Use_decl_annotations_ EXTERN_C static bool ShpSetupInlineHook(void* patch_address, UCHAR* shadow_exec_page, void** original_call_ptr)
// Builds a trampoline code for calling an original code and embeds 0xcc on the shadow_exec_page
{
    PAGED_CODE();

    const auto patch_size = ShpGetInstructionSize(patch_address);
    if (!patch_size) {
        return false;
    }

    // Build trampoline code (copied stub -> in the middle of original)
    const auto jmp_to_original = ShpMakeTrampolineCode(reinterpret_cast<UCHAR*>(patch_address) + patch_size);
#pragma warning(push)
#pragma warning(disable : 30030)  // Allocating executable POOL_TYPE memory
    const auto original_call = ExAllocatePoolWithTag(NonPagedPoolExecute, patch_size + sizeof(jmp_to_original), kHyperPlatformCommonPoolTag);
#pragma warning(pop)
    if (!original_call) {
        return false;
    }

    // Copy original code and embed jump code following original code
    RtlCopyMemory(original_call, patch_address, patch_size);
#pragma warning(push)
#pragma warning(disable : 6386)
    // Buffer overrun while writing to 'reinterpret_cast<UCHAR*>original_call+patch_size':  the writable size is
    // 'patch_size+sizeof((jmp_to_original))' bytes, but '15' bytes might be written.
    RtlCopyMemory(reinterpret_cast<UCHAR*>(original_call) + patch_size, &jmp_to_original, sizeof(jmp_to_original));
#pragma warning(pop)

    // install patch to shadow page
    static const UCHAR kBreakpoint[] = {
        0xcc,
    };
    RtlCopyMemory(shadow_exec_page + BYTE_OFFSET(patch_address), kBreakpoint, sizeof(kBreakpoint));

    KeInvalidateAllCaches();

    *original_call_ptr = original_call;
    return true;
}


_Use_decl_annotations_ EXTERN_C static SIZE_T ShpGetInstructionSize(void* address)// Returns a size of an instruction at the address
{
    PAGED_CODE();

    KFLOATING_SAVE float_save = {};
    auto status = KeSaveFloatingPointState(&float_save);// Save floating point state
    if (!NT_SUCCESS(status)) {
        return 0;
    }

    // Disassemble at most 15 bytes to get an instruction size
    csh handle = {};
    const auto mode = IsX64() ? CS_MODE_64 : CS_MODE_32;
    if (cs_open(CS_ARCH_X86, mode, &handle) != CS_ERR_OK) {
        KeRestoreFloatingPointState(&float_save);
        return 0;
    }

    static const auto kLongestInstSize = 15;
    cs_insn* instructions = nullptr;
    const auto count = cs_disasm(handle, reinterpret_cast<uint8_t*>(address), kLongestInstSize, reinterpret_cast<uint64_t>(address), 1, &instructions);
    if (count == 0) {
        cs_close(&handle);
        KeRestoreFloatingPointState(&float_save);
        return 0;
    }

    // Get a size of the first instruction
    const auto size = instructions[0].size;
    cs_free(instructions, count);
    cs_close(&handle);

    KeRestoreFloatingPointState(&float_save);// Restore floating point state
    return size;
}


_Use_decl_annotations_ EXTERN_C static TrampolineCode ShpMakeTrampolineCode(void* hook_handler)// Returns code bytes for inline hooking
{
    PAGED_CODE();

#if defined(_AMD64_)
    // 90               nop
    // ff2500000000     jmp     qword ptr cs:jmp_addr
    // jmp_addr:
    // 0000000000000000 dq 0
    return{
        0x90,
        {
            0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
        },
        hook_handler,
    };
#else
    // 90               nop
    // 6832e30582       push    offset nt!ExFreePoolWithTag + 0x2 (8205e332)
    // c3               ret
    return{
        0x90, 0x68, hook_handler, 0xc3,
    };
#endif
}


_Use_decl_annotations_ static HookInformation* ShpFindPatchInfoByPage(const SharedShadowHookData* shared_sh_data, void* address)
// Find a HookInformation instance by address
{
    const auto found = std::find_if(shared_sh_data->hooks.cbegin(), shared_sh_data->hooks.cend(), [address](const auto& info) {return PAGE_ALIGN(info->patch_address) == PAGE_ALIGN(address); });
    if (found == shared_sh_data->hooks.cend()) {
        return nullptr;
    }
    return found->get();
}


_Use_decl_annotations_ static HookInformation* ShpFindPatchInfoByAddress(const SharedShadowHookData* shared_sh_data, void* address)
// Find a HookInformation instance that are on the same page as the address
{
    auto found = std::find_if(shared_sh_data->hooks.cbegin(), shared_sh_data->hooks.cend(), [address](const auto& info) { return info->patch_address == address; });
    if (found == shared_sh_data->hooks.cend()) {
        return nullptr;
    }
    return found->get();
}


_Use_decl_annotations_ static void ShpEnablePageShadowingForExec(const HookInformation& info, EptData* ept_data)// Show a shadowed page for execution
{
    const auto ept_pt_entry = EptGetEptPtEntry(ept_data, UtilPaFromVa(info.patch_address));

    // Allow the VMM to redirect read and write access to the address by denying those accesses and handling them on EPT violation
    ept_pt_entry->fields.write_access = false;
    ept_pt_entry->fields.read_access = false;

    // Only execution is allowed on the adresss. Show the copied page for exec that has an actual breakpoint to the guest.
    ept_pt_entry->fields.physial_address = UtilPfnFromPa(info.pa_base_for_exec);

    UtilInveptGlobal();
}


_Use_decl_annotations_ static void ShpEnablePageShadowingForRW(const HookInformation& info, EptData* ept_data)
// Show a shadowed page for read and write
{
    const auto ept_pt_entry = EptGetEptPtEntry(ept_data, UtilPaFromVa(info.patch_address));

    // Allow a guest to read and write as well as execute the address.
    // Show the copied page for read/write that does not have an breakpoint but reflects all modification by a guest if that happened.
    ept_pt_entry->fields.write_access = true;
    ept_pt_entry->fields.read_access = true;
    ept_pt_entry->fields.physial_address = UtilPfnFromPa(info.pa_base_for_rw);

    UtilInveptGlobal();
}


_Use_decl_annotations_ static void ShpDisablePageShadowing(const HookInformation& info, EptData* ept_data)// Stop showing a shadow page
{
    const auto pa_base = UtilPaFromVa(PAGE_ALIGN(info.patch_address));
    const auto ept_pt_entry = EptGetEptPtEntry(ept_data, pa_base);
    ept_pt_entry->fields.write_access = true;
    ept_pt_entry->fields.read_access = true;
    ept_pt_entry->fields.physial_address = UtilPfnFromPa(pa_base);

    UtilInveptGlobal();
}


_Use_decl_annotations_ static void ShpSetMonitorTrapFlag(ShadowHookData* sh_data, bool enable)// Set MTF on the current processor
{
    VmxProcessorBasedControls vm_procctl = { static_cast<unsigned int>(UtilVmRead(VmcsField::kCpuBasedVmExecControl)) };
    vm_procctl.fields.monitor_trap_flag = enable;
    UtilVmWrite(VmcsField::kCpuBasedVmExecControl, vm_procctl.all);
}


_Use_decl_annotations_ static void ShpSaveLastHookInfo(ShadowHookData* sh_data, const HookInformation& info)
// Saves HookInformation as the last one for reusing it on up coming MTF VM-exit
{
    NT_ASSERT(!sh_data->last_hook_info);
    sh_data->last_hook_info = &info;
}


_Use_decl_annotations_ static const HookInformation* ShpRestoreLastHookInfo(ShadowHookData* sh_data)// Retrieves the last HookInformation
{
    NT_ASSERT(sh_data->last_hook_info);
    auto info = sh_data->last_hook_info;
    sh_data->last_hook_info = nullptr;
    return info;
}


_Use_decl_annotations_ static bool ShpIsShadowHookActive(const SharedShadowHookData* shared_sh_data)// Checks if DdiMon is already initialized
{
    return !!(shared_sh_data);
}


Page::Page() : page(reinterpret_cast<UCHAR*>(ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, kHyperPlatformCommonPoolTag)))
// Allocates a non-paged, page-aligned page. Issues bug check on failure
{
    if (!page) {
        HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kCritialPoolAllocationFailure, 0, 0, 0);
    }
}


Page::~Page()// De-allocates the allocated page
{
    ExFreePoolWithTag(page, kHyperPlatformCommonPoolTag);
}
