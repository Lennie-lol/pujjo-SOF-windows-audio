#!/usr/bin/env python3
"""Read-only PnP discovery regression: not a physical test."""
from pathlib import Path
src=(Path(__file__).resolve().parents[1]/"src/driver/Device.c").read_text()
caps=src.split("static NTSTATUS SofDiscoverHdaCaps(",1)[1].split("\n/**",1)[0]
prep=src.split("SofEvtDevicePrepareHardware(",1)[1].split("\n/**",1)[0]
assert "writel(" not in caps and "WRITE_REGISTER_" not in caps
assert "busInterface.SetBusData(" not in prep
assert prep.index("pciConfig.Command & PCI_ENABLE_MEMORY_SPACE") < prep.index("SofMapPreparedBars")
d0 = src.split("NTSTATUS\nSofEvtDeviceD0Entry(", 1)[1].split("\n/**", 1)[0]
assert "PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER" in d0
assert d0.index("busInterface.GetBusData(") < d0.index("busInterface.InterfaceDereference(")
assert d0.index("bytesRead != sizeof(command)") < d0.index("SofStartFirmwareBoot(")
assert "(command & required) != required" in d0 and "busInterface.SetBusData(" not in d0
assert "PCI Command updated" not in prep
assert "writel(" not in prep and "WRITE_REGISTER_" not in prep
assert "SofHardwareDevelopmentAllowed()" in prep
assert prep.index("!SofHardwareDevelopmentAllowed()") < prep.index("WdfFdoQueryForInterface")
assert "status = SofDiscoverHdaCaps(ctx);\n    if (!NT_SUCCESS(status)) goto PreparationFailed;" in prep
assert "PujjoReadMemoryBar(pciConfig.u.type0.BaseAddresses, 0" in prep
assert "PujjoReadMemoryBar(pciConfig.u.type0.BaseAddresses, 4" in prep
assert prep.index("SofSelectPciResources(ResourcesRaw, ResourcesTranslated") < prep.index("SofMapPreparedBars(ctx, bars)")
assert "memoryBarIndex" not in prep
assert "SofIpcInitialize(ctx)" in prep
assert "without CRST writes" in prep
assert "ctx->Bar0Length < SOF_HDA_REG_ICS + sizeof(USHORT)" in prep
assert "SOF_HDA_WAKESTS" not in prep and "IDispCodecPresent" not in prep
print("PASS: PCI/HDA resource discovery is read-only and checks controller status bounds")

# Diagnostic reporting must not open an SPB or GPIO target in default builds.
peripheral=(Path(__file__).resolve().parents[1]/"src/driver/peripheral/PujjoPeripheralKmdf.c").read_text()
match=peripheral.split("static BOOLEAN PjHardwareIdMatches(",1)[1].split("\n}",1)[0]
assert "ids[total - 1] != 0 || ids[total - 2] != 0" in match
assert "matched = TRUE;" in match and "return TRUE;" not in match
assert match.index("if (ids[pos++] != 0) return FALSE;") < match.index("return matched;")
prepare=peripheral.split("NTSTATUS PjEvtPrepareHardware(",1)[1].split("\n}",1)[0]
assert "Resource policy rejected:" in prepare and "Resource policy accepted:" in prepare
assert "connection class=" in prepare
assert prepare.index("if (result != PJ_POLICY_OK)") < prepare.index("ctx->Evidence = summary;") < prepare.index("Pj5682sSpbOpen(")
assert prepare.index("#if defined(PJ_TARGET_CODEC) && defined(PJ5682S_ENABLE_READONLY_PROBE)") < prepare.index("Pj5682sSpbOpen(")
assert "Pj1019" not in prepare
print("PASS: peripheral diagnostics report translated resources without default activation")

transport=(Path(__file__).resolve().parents[1]/"src/driver/peripheral/PujjoRt5682sSpbWindows.c").read_text()
control = transport.split("/* Private transport for typed operations.", 1)[1]
assert "WdfIoTargetSendWriteSynchronously" in control
assert "WdfIoTargetSendWriteSynchronously" not in transport.split("/* Private transport for typed operations.", 1)[0]
assert "#ifdef PJ5682S_ENABLE_CONTROL\n/* Private transport" in transport
assert "bus->write_only=NULL;" in transport
read=transport.split("static int Pj5682sSpbRead(",1)[1].split("\n}",1)[0]
for register in ("PJ5682S_DEVICE_ID_REGISTER","PJ5682S_VENDOR_ID_REGISTER","PJ5682S_VERSION_REGISTER"):
    assert read.index(register) < read.index("WdfIoTargetSendIoctlSynchronously")
assert "tx[0] != 0" in read and "txLen!=2" in read
# Both transport status and exact byte count must be checked before publishing.
assert "SpbTransferDirectionFromDevice, 0, observed, (ULONG)sizeof(observed)" in read
guard = "if (!NT_SUCCESS(status) || transferred != txLen + rxLen)"
assert read.index("WdfIoTargetSendIoctlSynchronously") < read.index(guard)
failure = read.split(guard, 1)[1].split("}", 1)[0]
assert "return -1;" in failure and "WdfWaitLockRelease(state->Lock);" in failure
assert read.index(guard) < read.index("rx[0] = observed[0];") < read.index("rx[1] = observed[1];")
assert read.count("rx[0] =") == 1 and read.count("rx[1] =") == 1
print("PASS: Windows SPB transport restricts physical transactions to identity reads")

# Compile the actual discovery function against a bounded fake MMIO map.
# No WDK code is executed here and no real device is accessed.
import os, subprocess, tempfile, shlex
hw=(Path(__file__).resolve().parents[1]/"src/driver/Hw.h").read_text()
names=("GCAP","LLCH","MAX_CAPS","CAP_ID_OFF","CAP_ID_MASK","CAP_NEXT_MASK",
       "PP_CAP_ID","SPIB_CAP_ID","ML_CAP_ID","REG_PP_PPCTL","REG_CL_SPBFIFO_SPBFCCTL")
defines="\n".join(next(line for line in hw.splitlines() if line.startswith("#define SOF_HDA_"+name+" ")) for name in names)
fixture=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef unsigned char *PUCHAR;
typedef int NTSTATUS;
#define _In_
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_DEVICE_CONFIGURATION_ERROR -2
#define STATUS_DEVICE_NOT_READY -3
#define DbgPrintEx(...) ((void)0)
typedef struct { PUCHAR Bar0VirtualAddress; ULONG Bar0Length;
 ULONG NumCaptureStreams, NumPlaybackStreams, PpCapOffset, SpbCapOffset, MlCapOffset;
} SOF_DEVICE_CONTEXT, *PSOF_DEVICE_CONTEXT;
static unsigned char regs[1024];
static ULONG limit;
static USHORT readw(PUCHAR p) { assert(p>=regs && p+2<=regs+limit); return (USHORT)(p[0]|((ULONG)p[1]<<8)); }
static ULONG readl(PUCHAR p) { assert(p>=regs && p+4<=regs+limit && ((p-regs)&3)==0); return (ULONG)p[0]|((ULONG)p[1]<<8)|((ULONG)p[2]<<16)|((ULONG)p[3]<<24); }
static void put(ULONG at, ULONG v) { for(unsigned i=0;i<4;++i)regs[at+i]=(unsigned char)(v>>(8*i)); }
'''
cases=r'''
static SOF_DEVICE_CONTEXT ctx;
static void init(void) {
 memset(regs,0,sizeof(regs)); limit=sizeof(regs);
 ctx=(SOF_DEVICE_CONTEXT){regs,limit,99,99,99,99,99};
 put(SOF_HDA_GCAP,0x55aa3200u);
}
static void bad(void) {
 assert(SofDiscoverHdaCaps(&ctx)<0);
 assert(!ctx.NumCaptureStreams && !ctx.NumPlaybackStreams && !ctx.PpCapOffset && !ctx.SpbCapOffset && !ctx.MlCapOffset);
}
static void cap(ULONG at,ULONG id,ULONG next) {put(at,(id<<SOF_HDA_CAP_ID_OFF)|next);}
int main(void) {
 init(); assert(SofDiscoverHdaCaps(&ctx)==0 && ctx.NumCaptureStreams==2 && ctx.NumPlaybackStreams==3 && !ctx.MlCapOffset);
 init();put(SOF_HDA_LLCH,0x100);cap(0x100,3,0x120);cap(0x120,4,0x140);cap(0x140,2,0);
 assert(SofDiscoverHdaCaps(&ctx)==0 && ctx.PpCapOffset==0x100 && ctx.SpbCapOffset==0x120 && ctx.MlCapOffset==0x140);
 init();put(SOF_HDA_GCAP,0xffff);bad();
 init();put(SOF_HDA_LLCH,0xffff);bad();
 init();put(SOF_HDA_LLCH,0x101);bad();
 init();put(SOF_HDA_LLCH,0x400);bad();
 init();put(SOF_HDA_LLCH,0x20);bad();
 init();put(SOF_HDA_LLCH,0x100);put(0x100,0xffffffffu);bad();
 init();put(SOF_HDA_LLCH,0x100);bad();
 init();put(SOF_HDA_LLCH,0x100);cap(0x100,3,0x100);bad();
 init();put(SOF_HDA_LLCH,0x100);cap(0x100,3,0x120);cap(0x120,4,0x100);bad();
 init();put(SOF_HDA_LLCH,0x100);cap(0x100,3,0x120);cap(0x120,3,0);bad();
 init();put(SOF_HDA_LLCH,0x3fc);cap(0x3fc,4,0);bad();
 init();put(SOF_HDA_LLCH,0x100);
 for(unsigned i=0;i<SOF_HDA_MAX_CAPS;++i)cap(0x100+i*4,9,0x104+i*4);
 bad();
 init();ctx.Bar0VirtualAddress=0;bad();
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="pj_hda_caps_") as d:
    source=Path(d)/"caps.c"
    exe=Path(d)/("caps.exe" if os.name=="nt" else "caps")
    source.write_text(fixture+defines+"\nstatic NTSTATUS SofDiscoverHdaCaps("+caps+"\n"+cases)
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",*shlex.split(os.environ.get("CFLAGS","")),str(source),"-o",str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=10)
print("PASS: malformed HDA capability chains fail closed in mocked MMIO execution")
