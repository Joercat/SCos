/* The Blackwell display functions this module reads.  Names and ids are the ones the in-tree registry
 * carried before this module existed, when they named a chip that nothing bound to - the row set is the
 * hand-authored extension of the `nvidia` family in kernel/drivers/gpu/gpu_ids.h, kept byte-identical to
 * it so that the two tables cannot disagree about what a device id means.  No id here implies drawing:
 * this driver identifies, and refuses to paint. */
    {0x10de, 0x2B85, 0, 0, 0}, /* GB202 [GeForce RTX 5090] */
    {0x10de, 0x2B87, 0, 0, 0}, /* GB202 [GeForce RTX 5090 D] */
    {0x10de, 0x2B8C, 0, 0, 0}, /* GB202 [GeForce RTX 5090 D V2] */
    {0x10de, 0x2C02, 0, 0, 0}, /* GB203 [GeForce RTX 5080] */
    {0x10de, 0x2C05, 0, 0, 0}, /* GB203 [GeForce RTX 5070 Ti] */
    {0x10de, 0x2C09, 0, 0, 0}, /* GB203 [GeForce RTX 5070] */
    {0x10de, 0x2C18, 0, 0, 0}, /* GB203M [GeForce RTX 5090 Max-Q / Mobile] */
    {0x10de, 0x2C19, 0, 0, 0}, /* GB203M [GeForce RTX 5080 Max-Q / Mobile] */
    {0x10de, 0x2C58, 0, 0, 0}, /* GB203M-X11 [GeForce RTX 5090 Max-Q / Mobile] */
    {0x10de, 0x2C59, 0, 0, 0}, /* GB203M-X9 [GeForce RTX 5080 Max-Q / Mobile] */
    {0x10de, 0x2D04, 0, 0, 0}, /* GB206 [GeForce RTX 5060 Ti] */
    {0x10de, 0x2D05, 0, 0, 0}, /* GB206 [GeForce RTX 5060] */
    {0x10de, 0x2D18, 0, 0, 0}, /* GB206M [GeForce RTX 5070 Max-Q / Mobile] */
    {0x10de, 0x2D19, 0, 0, 0}, /* GB206M [GeForce RTX 5060 Max-Q / Mobile] */
    {0x10de, 0x2D83, 0, 0, 0}, /* GB207 [GeForce RTX 5050] */
    {0x10de, 0x2D98, 0, 0, 0}, /* GB207M [GeForce RTX 5050 Max-Q / Mobile] */
    {0x10de, 0x2F04, 0, 0, 0}, /* GB205 [GeForce RTX 5070] */
    {0x10de, 0x2F06, 0, 0, 0}, /* GB205 [GeForce RTX 5060] */
    {0x10de, 0x2F18, 0, 0, 0}, /* GB205M [GeForce RTX 5070 Ti Mobile] */
