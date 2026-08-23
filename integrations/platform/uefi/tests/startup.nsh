@echo -off
echo Starting U-1 self-test...
fs0:\EFI\BOOT\BOOTX64.EFI
echo U-1 self-test returned.
reset -s
