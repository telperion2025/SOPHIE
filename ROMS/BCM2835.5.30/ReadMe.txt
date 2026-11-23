RISC OS Open: Raspberry Pi Stable Release 5.30
==============================================

This is the BCM2835 (Raspberry Pi) stable release 5.30 ROM image.

It has been tested and verified as running on the following combinations of
system on chip:

  BCM2835 (Pi Zero/ZeroW, Pi 1 model A/A+/B/B+, Compute Module 1)
  BCM2836 (Pi 2 up to and including revision 1.1)
  BCM2837 (Pi 2 revision 1.2 and later, Pi Zero2W, Pi 3 model A+/B/B+,
           Compute Module 3/3+)
  BCM2838 (Pi 4 model B, Pi 400 all-in-one, Compute Module 4)

Using this ROM image in your board should only be done if you are confident
that you know what you are doing!

Want to improve the OS?
-----------------------
We're collecting funds towards big improvements our favourite Open Source
operating system, and you can help too by contributing any amount big or small.

Go to
  https://www.riscosopen.org/bounty/

or if you prefer to get involved with the code directly see
  https://gitlab.riscosopen.org/

to see how it all works.


How to install the ROM image
============================

You'll need to obtain the corresponding firmware to control the VideoCore
processor that runs alongside the Arm that RISC OS runs on. This is included
along with a full set of disc based applications in the full SD card image
which can be downloaded from:

  https://www.riscosopen.org/content/downloads/raspberry-pi

For convenvience the firmware that was contemporary with the RISC OS 5.30
release is included;

  BOOTCODE/BIN
  FIXUP/DAT
  START/ELF
  FIXUP4/DAT   (only needed for Pi 4)
  START4/ELF   (only needed for Pi 4)

along with the licence terms under which Broadcom Corporation provide these
binaries.

CMOS RAM
========

The Raspberry Pi doesn't include any CMOS memory on the circuit board which
RISC OS expects to find to hold its essential configuration options needed
before the main !Boot application is run.

It is possible to seed the CMOS settings in one of two ways

  * Add a CMOS file to the SD card
    Make any configuration settings changes desired than use the *SaveCMOS
    command to capture them in a file, put this file on the SD card alongside
    the RISC OS ROM image. The SDCMOS module included in the operating system
    will update this file whenever a configuration change is made.
  * Do nothing
    If the hardware is not present, and you haven't put a CMOS file on the
    SD card, you'll get the built in defaults every time you turn on.

-- RISC OS Open
