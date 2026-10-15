/* Flash split for the 1 MB ESP8285s on ExpressLRS receivers, and every other
   1 MB ESP8266 target. */
/* sketch @0x40200000 (~967KB) (991216B) */
/* empty  @0x402F1FF0 (~4KB) (4112B) */
/* fs     @0x402F3000 (32KB) (32768B) */
/* eeprom @0x402FB000 (4KB) */
/* rfcal  @0x402FC000 (4KB) */
/* wifi   @0x402FD000 (12KB) */

/* The core's eagle.flash.1m64.ld, with the filesystem cut from 64 KB to 32 KB.

   Over-the-air updates are why. The ESP8266 Updater stages the new image in
   the gap between the end of the running sketch and the start of the
   filesystem, and refuses an upload that does not fit there. The board default
   (eagle.flash.1m256.ld) left about 220 KB of gap behind a 525 KB image, so
   not even the .bin.gz fit and OTA was simply broken. LittleFS here holds
   config.json (~1.5 KB) and maybe a /hardware.json (2 KB): 32 KB is plenty,
   and it is the same split ExpressLRS ships on these receivers.

   Even so, only the .bin.gz fits through OTA on a 1 MB part, never the plain
   .bin: the gap is under half the flash. package_release() in
   scripts/env_setup.py fails the build when the .bin.gz stops fitting. */

MEMORY
{
  dport0_0_seg :                        org = 0x3FF00000, len = 0x10
  dram0_0_seg :                         org = 0x3FFE8000, len = 0x14000
  irom0_0_seg :                         org = 0x40201010, len = 0xf1ff0
}

PROVIDE ( _FS_start = 0x402F3000 );
PROVIDE ( _FS_end = 0x402FB000 );
PROVIDE ( _FS_page = 0x100 );
PROVIDE ( _FS_block = 0x1000 );
PROVIDE ( _EEPROM_start = 0x402fb000 );

INCLUDE "local.eagle.app.v6.common.ld"
