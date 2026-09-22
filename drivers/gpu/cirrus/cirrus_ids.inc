/* The functions this module drives.  The device model's own id, and the only one whose bitBLT is
 * verified end to end here; the loader cross-checks these rows against the family table, so a module
 * cannot claim a chip the kernel did not detect. */
    {0x1013, 0x00B8, 0, 0, 0}, /* GD 5446 (PCI), 1994: the last Cirrus with a real bitBLT engine */
