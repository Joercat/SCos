{pkgs}: {
  deps = [
    pkgs.qemu_full
    pkgs.binutils-unwrapped
    pkgs.rPackages.matrixcalc
  ];
}
