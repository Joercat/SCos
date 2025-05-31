
{pkgs}: {
  deps = [
    pkgs.qemu_full
    pkgs.binutils-unwrapped
    pkgs.nasm
    pkgs.glibc_multi
    pkgs.gcc_multi
    pkgs.glibc.dev
    pkgs.gcc.cc
  ];
}
