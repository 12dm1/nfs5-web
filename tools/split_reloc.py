"""PU: nfsu2-web relocates split-screen operands here; PORSCHE.EXE needs none."""
def operand(value, ip=0, immediate=False):
    return f"0x{value & 0xffffffff:x}u"
