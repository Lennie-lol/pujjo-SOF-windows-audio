"""Extract the actual kernel DMA ownership functions for host regressions."""
from pathlib import Path
driver = Path(__file__).resolve().parents[1] / "src/driver"
loader = (driver / "Loader.c").read_text()
header = (driver / "Loader.h").read_text()
ledger = "\n".join(line for line in header.splitlines()
                   if line.startswith(("#define SOF_DMA_SLOT_MASK", "#define SOF_DMA_JOURNAL_")))
ledger += r"""
typedef int32_t LONG;
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG expected) {
    __atomic_compare_exchange_n(p,&expected,v,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    return expected;
}
static LONG InterlockedOr(volatile LONG *p,LONG v) {
    return __atomic_fetch_or(p,v,__ATOMIC_SEQ_CST);
}
"""
ledger += loader[loader.index("BOOLEAN SofOpenDmaPublicationGate("):
                 loader.index("/* Driver-owned page backing")]
