/* 
 * Valkyrie
 * Copyright (C) 2011, Stefano Teso
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <string.h>

#include "mach/hikaru/hikaru-gpu.h"
#include "mach/hikaru/hikaru-renderer.h"

/*
 * Overview
 * ========
 *
 * Unknown hardware, possibly tailor-made for the Hikaru by SEGA. All ICs are
 * branded SEGA, and the PCI IDs are as well. It is known to handle fire
 * and water effects quite well, but it's unlikely to be equipped with more
 * than a fixed-function pipeline (it was developed in 1998-1999 after all.)
 *
 * The GPU includes two distinct PCI IDs: 17C7:11DB and 17C3:11DB
 * The former is visible from the master SH-4 side, the latter from the slave
 * side.
 *
 * There are likely two different hardware revisions, the bootrom checks for
 * them by checking the reaction of the hardware (register 15002000) after
 * poking a few registers.
 *
 * The GPU(s) include at least:
 *
 *  - A command stream processor, which executes instructions in CMDRAM,
 *    with an etherogeneous 32-bit ISA and variable-length instructions.
 *    It is capable to call sub-routines, and so is likely to hold a
 *    stack somewhere (still to figure out where, though.)
 *
 *    My guess is that even and odd frames are processed by two different,
 *    identical processors.
 *
 *    The device is likely controlled by the MMIOs at 1500007x. The code
 *    is held in CMDRAM, which is at 14300000-143FFFFF in the master SH-4
 *    address space, and 480000-483FFFFF in bus address space.
 *
 *  - An indirect DMA-like device which is likely used to move texture
 *    data to/from TEXRAM, and is able to decode between texture formats on
 *    the fly.
 *
 *    The device can be accessed thru the MMIOs at 150000(0C|10|14).
 *
 *  - A FIFO-like device, yet to be figured out. The device is accessed
 *    through
 *
 *    The device can be accessed thru the MMIOs at 1A0400xx.
 *
 * Current Status
 * ==============
 *
 * Reversed stuff:
 *  + Preliminary memory map
 *  + Preliminary instruction execution
 *  + Very basic vertex information
 *  + Very basic texture information
 *  + A few IRQ and texturing related ports
 *
 * Missing stuff:
 *  - Matrix stack and proper viewport projection
 *	- translation is 
 *  - Does this thing make use of stack space? (See @0C0080E0)
 *  - Pretty much everything else (lighting, blending, etc.)
 *
 * Matrices
 * ========
 *
 * The hardware uses 4x3 matrices (see the 161 command), with the fourth
 * vector specifying translation.
 */

/* TODO: figure out what is 4Cxxxxxx */

/*
 * GPU MMIOs at 15000000
 * =====================
 *
 * Display Config
 * --------------
 *
 * 15000000 32-bit  W	UNK_15_00	= 0
 * 15000004 32-bit  W	DISP_MODE	Display mode
 *					 0 = hi-res (640x480)
 *					 1 = lo-res (496x384)
 *					See @0C001AD8, @0C00792C)
 * 15000008 32-bit  W	UNK_15_08	= 0
 *
 * Indirect DMA/Texture Conversion MMIOs
 * -------------------------------------
 *
 * 1500000C  32-bit  W	IDMA_ADDR	GPU 15 IDMA table address in CMDRAM
 * 15000010  32-bit RW	IDMA_DIFF	GPU 15 IDMA # of entries to process
 * 15000014  32-bit RW	IDMA_CTL	GPU 15 IDMA exec/busy (bit 0)
 *
 * GPU 15 Unknown Config A
 * -----------------------
 *
 * 15000018  32-bit  W	CONFIG_15_UNK	= 0         
 * 1500001C  32-bit  W	CONFIG_15_UNK	= 0x00040000
 * 15000020  32-bit  W	CONFIG_15_UNK	= 0x00048000
 * 15000024  32-bit  W	CONFIG_15_UNK	= 0x00058000
 * 15000028  32-bit  W	CONFIG_15_UNK	= 0x00007800
 * 1500002C  32-bit  W	CONFIG_15_UNK	= 0x0007FE00
 * 15000030  32-bit  W	CONFIG_15_UNK	= 0         
 * 15000034  32-bit  W	CONFIG_15_UNK	= 0x00005000
 *
 * GPU 15 Unknown Config B
 * -----------------------
 *
 * 15000038  32-bit  W	CONFIG_15_UNK	= 0x00080000
 * 1500003C  32-bit  W	CONFIG_15_UNK	= 0x000C0000
 * 15000040  32-bit  W	CONFIG_15_UNK	= 0x000C8000
 * 15000044  32-bit  W	CONFIG_15_UNK	= 0x000D8000
 * 15000048  32-bit  W	CONFIG_15_UNK	= 0x0000F800
 * 1500004C  32-bit  W	CONFIG_15_UNK	= 0x000FFE00
 * 15000050  32-bit  W	CONFIG_15_UNK	= 0x00008000
 * 15000054  32-bit  W	CONFIG_15_UNK	= 0x0000D000
 *
 * Note: same as Config A, but offset by +80000 or +8000.
 *
 * Unknown
 * -------
 *
 * 15000058  32-bit  W	GPU_15_CTL_58	= 6; related to 1A000024 b0
 *
 * GPU 15 Program Control
 * ----------------------
 *
 * 15000070  32-bit  W	PROGRAM_START	= 48000100
 * 15000074  32-bit  W	PROGRAM_UNK_1	= 483F0100 \ See 15000030, 15000050
 * 15000078  32-bit  W	PROGRAM_UNK_2	= 483F8100 /
 * 1500007C  32-bit  W	UNK_FLAG	Abort execution when flipped 0, 1 are
 *					written? See @0C006AFC
 *
 * Note: perhaps program execution is to output stuff to the addresses
 * indicated by 15000074 and 15000078 as a side effect? They could also
 * specify the stack for two independent execution threads.
 *
 * Unknown
 * -------
 *
 * 15000080  32-bit  W	GPU_15_CTL_80	= 6
 *
 * Interrupt Control
 * -----------------
 *
 * 15000084  32-bit   W		GPU IRQ Mask
 * 15000088  32-bit  RW		GPU IRQ Status
 *				 0x80 = GPU 1A IRQ fired
 *				 0x40 = Unknown
 *				 0x20 = Unknown
 *				 0x10 = Unknown
 *				 0x08 = Unknown
 *				 0x04 = GPU 15 finished; see @0C0018B4
 *				 0x02 = Unknown
 *				 0x01 = GPU 15 15000014-related (hblank/vblank?); see @0C006C04
 *				All bits are AND'ed on write
 *
 * Unknown
 * -------
 *
 * 1500008C             l  W    = 0x02020202 |
 * 15000090             l  W    = 0          | @0C001A82
 * 15000094             l  W    = 0          |
 * 15000098             l  W    = 0x02020202 |
 *
 * 15002000             l R     b0 is related to 1A00024:b0
 *				Possibly the GPU status regs
 *				Used to determine the HARDWARE VERSION:
 *				 - 0=older
 *				 - 1=newer, see PH:@0C01220C
 *				Probably this bit wasn't present in the older
 *				HW version.
 *
 * 15002800 32-bit  R	Unknown
 * 15002804 32-bit  R	Unknown
 * 15002808 32-bit  R	Unknown
 * 1500280C 32-bit  R	Unknown
 * 15002810 32-bit  R	Unknown
 * 15002814 32-bit  R	Unknown
 * 15002820 32-bit  R	Unknown
 * 15002824 32-bit  R	Unknown
 * 15002840 32-bit  R	Unknown
 * 15002844 32-bit  R	Unknown
 * 15002848 32-bit  R	Unknown
 *
 * 1502C100 32-bit W	Unknown, = 9
 * 1502C104 32-bit W	Unknown, = 6
 *
 * 15040E00  32-bit W	Unknown, = 0
 */

/* GPU MMIOs at 18001000
 * =====================
 *
 * NOTE: these ports are always read twice.
 *
 * 18001000	32-bit	RO	PCI ID: 17C7:11DB, a SEGA ID. See @0C0019AE
 * 18001004	32-bit	WO	= 2
 * /
 * 18001010	32-bit	WO	= 0xF2000000 Look like addresses, see 15000018+
 * 18001014	32-bit	WO	= 0xF2040000
 * 18001018	32-bit	WO	= 0xF2080000
 * 1800101C	32-bit	WO	= 0xF3000000
 */

/* GPU MMIOs at 1A000000
 * =====================
 *
 * NOTE: these ports are always read twice.
 *
 * Unknown
 * -------
 *
 * 1A000000	32-bit	 W 	GPU 1A Enable A; b0 = enable; See @0C0069E0, @0C006AFC
 * 1A000004	32-bit	 W 	GPU 1A Enable B; b0 = enable; See @0C0069E0, @0C006AFC
 *
 * Interrupt Control
 * -----------------
 *
 * 1A000008	32-bit	 W 	IRQ 1A Source 0
 * 1A00000C	32-bit	 W 	IRQ 1A Source 1; GPU 1A finished
 * 1A000010	32-bit	 W 	IRQ 1A Source 2
 * 1A000014	32-bit	 W 	IRQ 1A Source 3
 * 1A000018	32-bit	RW	IRQ 1A Status
 *				Four bits; bit n indicates the status of the
 *				IRQ governed by register 1A000008+(n*4)
 *
 * Note: when any of these bits is set, bit 7 of 15000088 is set.
 *
 * Note: may be related to 1A0000C4, see @0C001ED0.
 *
 * Unknown
 * -------
 *
 * 1A00001C	32-bit	RO	Unknown status
 *				 bits 23, 24 affect the argument of command 0781 
 *				 affects how much stuff is sent to the TEX FIFO in pharrier           \ both get written to the same GBR (0C00F070.w)
 *				Note: bits may be related to 48xxxxxx and 4Cxxxxxx
 * 1A000020	32-bit	RO	Unknown status
 *				 bit 0 = frame type; See @0C008130, selects the GPRs used for GPU upload /
 *
 * 1A000024	32-bit	RO	b0 is related to:
 *				 - 15000058 bits 0,1 and GPU jump instructions, see @0C0018B4
 *				 - 15002000 bit 0, see @?
 *				 - HW version, see @?
 *				 - @0C0069E8 loops while the bit is set
 *				 - it is set on frame change
 *				 - Also related to GPU texture upload (acts as a busy bit); see SN-ROM:@0C070C9C
 *
 * Display Config
 * --------------
 *                                             ----------------------
 *                                              640x480      496x377	AIRTRIX
 *                                             ----------------------
 * 1A000080             l  W    = 0x0000027F   639          818		00000332
 * 1A000084             l  W    = 0x000001A0   416          528		00000210
 * 1A000088             l  W    = 0x02680078   616 | 120    798 | 158	031E009E
 * 1A00008C             l  W    = 0x0196001D \ 406 |  29    516 |  36	02040024
 * 1A000090             l  W    = 0x02400000 | 576 |   0    728 |   0	02D80000
 * 1A000094             l  W    = 0x00000040 |   0 |  64      0 |  91	0000005B
 * 1A000098             l  W    = 0x00000003 |   0 |   3      0 |   3	00000003
 * 1A00009C             l  W    = 0x00000075 |   0 | 117      0 | 155	0000009B
 * 1A0000A0             l  W    = 0x00000198 /   0 | 408      0 | 574	0000023E
 * 1A0000A4             l  W    = 0x001D0194 \  29 | 404     36 | 514	00240202
 * 1A0000A8             l  W    = 0x00000195 |   0 | 405      0 | 515	00000203
 * 1A0000AC             l  W    = 0x00000000 |   0 |   0      0 |   0	00000000
 * 1A0000B0             l  W    = 0x00000000 |   0 |   0      0 |   0	00000000
 * 1A0000B4             l  W    = 0x00000000 |   0 |   0      0 |   0	00000000
 * 1A0000B8             l  W    = 0x00000179 /   0 | 377      0 | 416	000001A0
 * 1A0000BC             l  W    = 0x00000008     0 |   8      0 |   8	00000008
 * 1A0000C0             l  W    = 0x01960000   406 |   0      0 | 516	02040000
 *
 * Note: my gutter feeling is that these register specify operations that must
 * be performed at the rasterization stage to the whole contents of the frame
 * buffer.
 *
 * Unknown
 * -------
 *
 * 1A0000C4             l  W    = 6		Unknown control
 * 1A0000D0		l  W	= 1		Unknown control
 *
 * Texture RAM Control
 * -------------------
 *
 * 1A000100             l RW    Enables 180-200 Texture RAM Control/Processing/GPU access
 *				See @0C007d00 ,PH:@0C01A0F8, PH@0C01A10C; Texture stuff
 *				Perhaps the controls here decide the texture format/decoding?
 *
 * Texture RAM Control A & B
 * -------------------------
 *
 * 1A000180-1A0001BF    l RW    REGS A, 16 registers
 * 1A000200-1A00023F    l RW    REGS B, 16 registers
 *
 *     The UNIT's come in pairs: 9 LSBs + other, see PH:@0C01A860.
 *
 *     +0x34 lower 2 bits (at least) turn on/off a unit. See PH:@0C01A124.
 *     It uses the same (R4 < 2) check as PH:@0C01A860.
 *
 *      180             l RW    = 0x00000 \ UNIT 0                          \
 *      184             l RW    = 0x3BF3F /                                 | TEXRAM
 *      188             l RW    = 0x40000 \ UNIT 1                          | addresses in
 *      18C             l RW    = 0x7BF3F /                                 | 8-byte units
 *      190             l RW    = 0x00140 \ UNIT 2 [Reserved? See pharrier] |
 *      194             l RW    = 0x3BFDF /                                 | lower 9 bits only
 *      198             l RW    = 0x40140 \ UNIT 3 [Reserved? See pharrier] | see PH:@0C01A860
 *      19C             l RW    = 0x7BFDF /                                 /
 *      1A0             l RW    = 0 \ UNIT 0 CONTROL
 *      1A4             l RW    = 0 /
 *      1A8             l RW    = 0 \ UNIT 1 CONTROL
 *      1AC             l RW    = 0 /
 *      1B0             l RW    = 1 \ UNIT 2 CONTROL // a bitfield: see @0C007D60
 *      1B4             l RW    = 1 /
 *      1B8             l RW    = 3 \ UNIT 3 CONTROL
 *      1BC             l RW    = 6,0 /              // 6 to turn on, 0 to turn off; bitfield
 *
 *	These could be the TEXRAM content setup.
 *
 *      200-21C         are identical to 180-19C + 0x80000 [GPU CMD RAM OFFSET DIFF BETWEEN ODD/EVEN FRAMES!]
 *      220-23C         are identical to 1A0-1BC
 *
 *      The fact that there are four of these guys may be related to the
 *      fact that there are four different IRQ causes in 1A000018 -- not likely
 *
 *      Point is that the +180 regs seem to be used in even frames, +200 regs in
 *      odd ones. 200+ is written to 180+ sometimes.
 *
 *      Related to TEXTURING.
 *
 * Unknown
 * -------
 *
 * 1A020000 32-bit  W	"SEGA" is written here; see @0C001A58
 *
 * Texture FIFO
 * ------------
 *
 * 1A040000 32-bit  W	Texture FIFO
 * 1A040004 32-bit  W	Texture FIFO
 * 1A040008 32-bit  W	Texture FIFO
 * 1A04000C 32-bit  W	Texture FIFO
 *
 * Note: this thing could be not related to texturing, but it's definitely
 * a FIFO.
 *
 * Unknown
 * -------
 *
 * 1A08006C 32-bit R	Unknown
 *
 * 1A0A1600             l  W    1 [seems related to 15040E00, see pharrier]
 */

typedef struct {
	vec3s_t unk;
} _811_params_t;

typedef struct {
	vec3b_t unk;
	unsigned sign;
} _991_params_t;

typedef struct {
	float persp_x;
	float persp_y;
	float unk;
} _021_params_t;

typedef struct {
	vec2s_t center;
	vec2s_t extents_x;
	vec2s_t extents_y;
} _221_params_t;

typedef struct {
	unsigned depth_func;
	float depth_near;
	float depth_far;
} _421_params_t;

typedef struct {
	uint32_t enabled;
	uint32_t unk_n;
	uint32_t unk_b;
	vec4b_t	color;
	float inv_delta;
	float inv_max;
} _621_params_t;

typedef struct {
	_811_params_t _811_params; /* Unknown Params */
	_991_params_t _991_params; /* Unknown Params */
	_021_params_t _021_params; /* Aspect Ratio */
	_221_params_t _221_params; /* Extents */
	_421_params_t _421_params; /* Depth Params */
	_621_params_t _621_params; /* Unknown Params */
} viewport_state_t;

typedef struct {
	vk_device_t base;

	vk_buffer_t *cmdram;
	vk_buffer_t *texram;

	uint8_t regs_15[0x100];
	uint8_t regs_18[0x100];
	uint8_t regs_1A[0x104];
	uint8_t regs_1A_unit[2][0x40];
	uint8_t regs_1A_fifo[0x10];

	bool is_running;
	bool fifo_is_running;

	unsigned frame_type;
	uint32_t pc;
	uint32_t sp[2];
	int cycles;

	viewport_state_t	vp_scratch;
	viewport_state_t	vp[8];

	/* TODO: color state */
	/* TODO: texture state */

	mtx4x3f_t mtx_scratch;
	mtx4x3f_t mtx[8];

	vec3f_t vertex_buffer[3];
	int vertex_index;

} hikaru_gpu_t;

#define REG15(addr_)	(*(uint32_t *) &gpu->regs_15[(addr_) & 0xFF])
#define REG18(addr_)	(*(uint32_t *) &gpu->regs_18[(addr_) & 0xFF])
#define REG1A(addr_)	(*(uint32_t *) &gpu->regs_1A[(addr_) & 0x1FF])
#define REG1AUNIT(n,a)	(*(uint32_t *) &gpu->regs_1A_unit[n][(a) & 0x3F])
#define REG1AFIFO(a)	(*(uint32_t *) &gpu->regs_1A_fifo[(a) & 0xF])

const char *
hikaru_gpu_get_debug_str (vk_device_t *dev)
{
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) dev;
	static char out[256];

	sprintf (out, "@%08X %u 15:58=%u 1A:24=%u 15:84=%X 15:88=%X 1A:18=%X",
	         gpu->pc, (unsigned) gpu->is_running,
	         REG15 (0x58), REG1A (0x24),
	         REG15 (0x84), REG15 (0x88), REG1A (0x18));

	return out;
}

static void
hikaru_gpu_print_state (hikaru_gpu_t *gpu)
{
	unsigned i;
	printf (" ==== GPU ====\n");
	for (i = 0; i < 0x9C; i += 4)
		printf ("%08X = %08X\n", 0x15000000 + i, REG15(i));
	for (i = 0; i < 0x104; i+= 4)
		printf ("%08X = %08X\n", 0x1A000000 + i, REG1A(i));
	for (i = 0; i < 0x40; i+= 4)
		printf ("%08X = %08X\n", 0x1A000180 + i, REG1AUNIT(0,i));
	for (i = 0; i < 0x40; i+= 4)
		printf ("%08X = %08X\n", 0x1A000200 + i, REG1AUNIT(1,i));
	printf ("\n");
}

/*
 * GPU Address Space
 * =================
 *
 * The GPU has an internal memory map. Here is what it looks like:
 *
 * 101C0000-10600000 * 4 ---> 41070000-417FFFFF, 8 MB region (textures?)
 * 12000000-120FFFFF * 4 ---> 48000000-483FFFFF, 4 MB region (commands)
 * 13000000-130FFFFF * 4 ---> 4C000000-4C3FFFFF, 4 MB region (unknown, commands mirror?)
 *
 * GPU Instructions
 * ================
 *
 * Each GPU instruction is made of 1, 2, 4, or 8 32-bit words. The opcode is
 * specified by the lower 12 bits of the first word. The meaning of the values
 * is determined by the opcode as follows:
 */

/* GPU IRQs
 * ========
 *
 * IRQs are signalled to the main SH-4 through IRL2: priority 7, INTEVT 0x300,
 * vector 0x220. At the same time, bit 4 of the master SH-4 Port A is cleared
 * (it is active low).
 *
 * The two registers 15000088 and 1A000018 signal 8 and 4 different IRQs
 * causes, respectively. When any of the IRQs in 1A000018 is raised, bit 7 of
 * 15000088 is set. 15000084 is the IRQ mask register, and is applied to
 * 15000088.
 *
 * Bits in 15000088:
 *
 *  01, bit 0 = GPU 15 indirect DMA done
 *  02, bit 1 = Unknown but required for the bootrom to work
 *  04, bit 2 = GPU 15 done / ready
 *  08, bit 3 = Unknown
 *  10, bit 4 = Unknown
 *  20, bit 5 = Unknown
 *  40, bit 6 = Unknown
 *  80, bit 7 = 1A0000xx IRQ mirror
 *
 * Bits in 1A000088:
 *
 *  01, bit 0 = Unknown
 *  02, bit 1 = Vblank-out or Hblank-out [1]
 *  04, bit 2 = GPU 1A done / ready
 *  08, bit 4 = Unknown
 *
 * IRQs at 1A000018 are most likely related to Texture/FB operations, that is,
 * anything related to the 1A00xxxx registers (including texture FIFO, etc.)
 * When raised, they set bit 8 in 15000088.
 *
 * [1] This bit is checked in 0C001C08 and updates (0, GBR) and implies
 *     1A000000 = 1.
 */

typedef enum {
	_15_IRQ_IDMA	= (1 << 0),
	_15_IRQ_VBLANK	= (1 << 1),
	_15_IRQ_DONE	= (1 << 2),
	_15_IRQ_UNK3	= (1 << 3),
	_15_IRQ_UNK4	= (1 << 4),
	_15_IRQ_UNK5	= (1 << 5),
	_15_IRQ_UNK6	= (1 << 6),
	_15_IRQ_1A	= (1 << 7)
} _15_irq_t;

typedef enum {
	_1A_IRQ_UNK0	= (1 << 0),
	_1A_IRQ_VBLANK	= (1 << 1),
	_1A_IRQ_DONE	= (1 << 2),
	_1A_IRQ_UNK3	= (1 << 3)
} _1a_irq_t;

static void
hikaru_gpu_update_irqs (hikaru_gpu_t *gpu) 
{
	vk_cpu_t *cpu = ((hikaru_t *) gpu->base.mach)->sh_current;
	/* Update 1A000018 from 1A0000[08,0C,10,14] */
	REG1A (0x18) = (REG1A (0x18) & ~0xF) |
	               (REG1A (0x08) & 1) |
	               ((REG1A (0x0C) & 1) << 1) |
	               ((REG1A (0x10) & 1) << 2) |
	               ((REG1A (0x14) & 1) << 3);

	/* Update 15000088 bit 7 from 1A000018 */
	if (REG1A (0x18) & 0xF)
		REG15 (0x88) |= 0x80;

	/* Raise IRL2 and lower bit 5 of the PDTRA, if the IRQs are
	 * not masked. */
	if (REG15 (0x88) & REG15 (0x84)) {
		VK_CPU_LOG (cpu, " ## sending GPU IRQ to CPU: %02X/%02X",
		            REG15 (0x84), REG15 (0x88));
		hikaru_raise_irq (gpu->base.mach, 2, 0x0040);
	}
}

static void
hikaru_gpu_raise_irq (hikaru_gpu_t *gpu, uint32_t _15, uint32_t _1A)
{
	vk_cpu_t *cpu = ((hikaru_t *) gpu->base.mach)->sh_current;
	VK_CPU_LOG (cpu, " ### raising GPU IRQs 15:%02X 1A:%02X", _15, _1A);
	if (_1A & 1)
		REG1A (0x08) |= 1;
	if (_1A & 2)
		REG1A (0x0C) |= 1;
	if (_1A & 4)
		REG1A (0x10) |= 1;
	if (_1A & 8)
		REG1A (0x14) |= 1;
	REG15 (0x88) |= _15;
	hikaru_gpu_update_irqs (gpu);
}

/* Texture RAM
 * ===========
 *
 * Located at 1B000000-1B7FFFFF in the master SH-4 address space, 8MB large;
 * it is a single (double?) sheet of texel data. Supported texture formats
 * include RGBA4444, RGB565, RGBA5551, RGBA8888.
 *
 * It looks like the sheet has an (1 << 11) = 8192 bytes pitch.  See
 * PH:@0C01A242.
 */

#define TEXRAM_ROW_PITCH	(1 << 11)

static inline uint16_t
get_texel16 (hikaru_gpu_t *gpu, unsigned x, unsigned y)
{
	unsigned yoffs = y * TEXRAM_ROW_PITCH;
	unsigned xoffs = (x * 2) & (TEXRAM_ROW_PITCH - 1);
	return vk_buffer_get (gpu->texram, 2, yoffs + xoffs);
}

static inline uint32_t
get_texel32 (hikaru_gpu_t *gpu, unsigned x, unsigned y)
{
	unsigned yoffs = y * TEXRAM_ROW_PITCH;
	unsigned xoffs = (x * 4) & (TEXRAM_ROW_PITCH - 1);
	return vk_buffer_get (gpu->texram, 4, yoffs + xoffs);
}

/*
 * GPU Indirect DMA
 * ================
 *
 * Register 1500000C points to a table in GPU CMDRAM, defaulting to
 * 483FC000. Each entry has this format:
 *	
 *	3FC000: 48300000	data address
 *	3FC004: 00002000	lenght (in bytes)
 *	3FC008: 0812C080	unknown (bitfield)
 *	3FC00C: 00000000	unknown	(byte)
 *
 * Data can be located (at least) at 48xxxxxx (CMDRAM) or at 41xxxxxx
 * (slave RAM?).
 *
 * During the bootrom life-cycle, the data address to texture-like data (the
 * not-yet-converted ASCII texture.) However, the bootrom uploads this
 * texture independently to TEXRAM by performing the format conversion
 * manually (RGBA1 to RGBA4); it does however use the GPU IDMA mechanism
 * too. I don't know why.
 *
 * The third and fourth parameters decide the type of operation to do. Their
 * format is still unknown.
 *
 * Note: C080 and x812 are also used as parameters for the `Set X' GPU
 * command. No idea if there is any relation. Possibly texture format?
 *
 * Note: GPU 15 IDMA fires GPU 15 IRQ 1 when done.
 */

static void
hikaru_gpu_step_idma (hikaru_gpu_t *gpu)
{
	/* Step the GPU 15 indirect DMA thing */
	uint32_t entry[4], addr;

	/* If there's no entries still to do, return */
	if (!REG15 (0x10))
		return;

	VK_LOG (" ### GPU 15 IDMA status is = [ %08X %08X %08X ]",
	        REG15 (0x0C), REG15 (0x10), REG15 (0x14));

	VK_ASSERT ((REG15 (0x0C) >> 24) == 0x48);

	/* Read the IDMA table address in CMDRAM */
	addr = (REG15 (0x0C) & 0xFFFFFF);

	entry[0] = vk_buffer_get (gpu->cmdram, 4, addr+0x0); // address
	entry[1] = vk_buffer_get (gpu->cmdram, 4, addr+0x4); // length
	entry[2] = vk_buffer_get (gpu->cmdram, 4, addr+0x8); // two words
	entry[3] = vk_buffer_get (gpu->cmdram, 4, addr+0xC); // byte, unknown

	VK_LOG (" ## GPU 15 IDMA entry = [ %08X %08x %08X %08X ]",
	        entry[0], entry[1], entry[2], entry[3]);

	/* If the entry supplies a positive lenght, process it */
	if (entry[1]) {
		/* XXX actually process it ... */
		REG15 (0x0C) += 0x10;
		REG15 (0x10) --;
	}

	/* XXX note that the code assumes that the IDMA may stop even if
	 * there are still unprocessed entries. This probably means that
	 * the IDMA somehow stops processing the any other GPU IRQ fires */

	/* If there are no more entries, stop */
	if (REG15 (0x10) == 0) {
		/* XXX I don't think it actually gets overwritten considering
		 * that the IRL2 handler does it itself */
		/* REG15 (0x14) = 0; */
		hikaru_gpu_raise_irq (gpu, _15_IRQ_IDMA, 0);
	}

	VK_LOG (" ### GPU 15 IDMA status became = [ %08X %08X %08X ]",
	        REG15 (0x0C), REG15 (0x10), REG15 (0x14));
}

/*
static void
print_vertex_buffer (hikaru_gpu_t *gpu)
{
	int i;
	for (i = 0; i < 3; i++)
		printf (" VERTEX %u = { %f %f %f }\n",
		        i,
		        gpu->vertex_buffer[i].x[0],
		        gpu->vertex_buffer[i].x[1],
		        gpu->vertex_buffer[i].x[2]);
}
*/

static void
append_vertex (hikaru_gpu_t *gpu, vec3f_t *src)
{
	vec3f_t *dst = &gpu->vertex_buffer[gpu->vertex_index];
	*dst = *src;
	dst->x[0] /= 640.0f;
	dst->x[1] /= 480.0f;
	dst->x[1] += 1.0f;
	gpu->vertex_index = (gpu->vertex_index + 1) % 3;
}

static int
get_vertex_index (int i)
{
	return (i < 0) ? (i + 3) : i;
}

static vec2f_t
uv_hack (vec2s_t *in)
{
	vec2f_t out;
	out.x[0] = in->x[0] / 256.0f;
	out.x[1] = in->x[1] / 256.0f;
	return out;
}

static void
draw_tri (hikaru_gpu_t *gpu, vec2s_t *uv0, vec2s_t *uv1, vec2s_t *uv2)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) gpu->base.mach->renderer;
	vec2f_t uv[3];

	int i0 = get_vertex_index (gpu->vertex_index - 1);
	int i1 = get_vertex_index (gpu->vertex_index - 2);
	int i2 = get_vertex_index (gpu->vertex_index - 3);

	uv[0] = uv_hack (uv0);
	uv[1] = uv_hack (uv1);
	uv[2] = uv_hack (uv2);

	hikaru_renderer_draw_tri (hr,
	                          &gpu->vertex_buffer[i0],
	                          &gpu->vertex_buffer[i1],
	                          &gpu->vertex_buffer[i2],
	                          &uv[0], &uv[1], &uv[2]);
}

static bool
cp_is_valid_addr (uint32_t addr)
{
	return (addr >= 0x48000000 && addr <= 0x483FFFFF) ||
	       (addr >= 0x4C000000 && addr <= 0x4C3FFFFF);
}

static void
cp_push_pc (hikaru_gpu_t *gpu)
{
	unsigned i = gpu->frame_type;
	vk_buffer_put (gpu->cmdram, 4, gpu->sp[i] & 0xFFFFFF, gpu->pc);
	gpu->sp[i] -= 4;
}

static void
cp_pop_pc (hikaru_gpu_t *gpu)
{
	unsigned i = gpu->frame_type;
	gpu->sp[i] += 4;
	gpu->pc = vk_buffer_get (gpu->cmdram, 4, gpu->sp[i] & 0xFFFFFF) + 8;
}

static int
hikaru_gpu_exec_one (hikaru_gpu_t *gpu)
{
	uint32_t inst[8];
	unsigned i;

	VK_ASSERT (cp_is_valid_addr (gpu->pc));
	VK_ASSERT (cp_is_valid_addr (gpu->sp[1]));
	VK_ASSERT (cp_is_valid_addr (gpu->sp[0]));

	/* XXX this is not exactly ideal; change the CMDRAM to an uint32_t
	 * buffer */
	for (i = 0; i < 8; i++)
		inst[i] = vk_buffer_get (gpu->cmdram, 4,
		                         (gpu->pc + i * 4) & 0xFFFFFF);

	switch (inst[0] & 0xFFF) {

	/* Flow Control */

	case 0x000:
		/* 000	Nop
		 *
		 * +00	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 */
		VK_LOG ("%08X: Nop [%08X]", gpu->pc, inst[0]);
		VK_ASSERT (inst[0] == 0);
		gpu->pc += 4;
		break;
	case 0x012:
		/* 012	Jump
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa		a = Address in 32-bit words
		 */
		{
			uint32_t addr = inst[1] * 4;
			VK_LOG ("%08X: Jump [%08X] %08X",
			        gpu->pc, inst[0], addr);
			VK_ASSERT ((inst[0] & ~0xFFF) == 0);
			VK_ASSERT ((addr >= 0x48000000) &&
			           (addr <= 0x483FFFFF));
			gpu->pc = addr;
		}
		break;
	case 0x052:
		/* 052	Call
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa		a = Address in 32-bit words
		 */
		{
			uint32_t addr = inst[1] * 4;

			VK_LOG ("%08X: Call [%08X] %08X",
			        gpu->pc, inst[0], addr);

			VK_ASSERT ((inst[0] & ~0xFFF) == 0);
			VK_ASSERT ((addr >= 0x4C000000) &&
			           (addr <= 0x4C3FFFFF));
			cp_push_pc (gpu);
			gpu->pc = addr;

		}
		break;
	case 0x082:
		/* 082	Return
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 */
		VK_LOG ("%08X: Return [%08X]",
		        gpu->pc, inst[0]);
		VK_ASSERT ((inst[0] & ~0xFFF) == 0);
		cp_pop_pc (gpu);
		break;
	case 0x1C2:
		/* 1C2	Kill
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 */
		VK_LOG ("%08X: Kill [%08X]",
		        gpu->pc, inst[0]);
		VK_ASSERT (!(inst[0] & 0xFFFFF000));
		gpu->is_running = false;
		gpu->pc += 4;
		return 1;

	/* Frame Control */

	case 0x781:
		/* 781	Sync
		 *
		 *	---- aabb ---- mmnn ---- oooo oooo oooo		o = Opcode, a, b, m, n = Unknown
		 *
		 * See @0C0065D6, PH:@0C016336
		 */
		{
			unsigned a, b, m, n;
			a = (inst[0] >> 26) & 3;
			b = (inst[0] >> 24) & 3;
			m = (inst[0] >> 18) & 3;
			n = (inst[0] >> 16) & 3;

			VK_LOG ("%08X: Sync [%08X] <%u %u %u %u>",
			        gpu->pc, inst[0], a, b, n, m);

			VK_ASSERT ((inst[0] & 0xF0F0F000) == 0);

			gpu->pc += 4;
		}
		break;

	/* Clear Primitives */

	case 0x154:
		/* 154	Clear Unknown A */
		{
			unsigned n, a, b, c, d;
			n = (inst[0] >> 16) & 0xFF;
			a = inst[1] & 0xFF;
			b = (inst[1] >> 8) & 0xFF;
			c = (inst[1] >> 16) & 0xFF;
			d = (inst[1] >> 24) & 0xFF;
			VK_LOG ("%08X: Clear Unknown A [%08X %08X] %u <%X %X %X %X>",
			        gpu->pc, inst[0], inst[1], n, a, b, c, d);
			gpu->pc += 8;
		}
		break;
	case 0x194:
		/* 194	Clear Unknown B */
		{
			unsigned n, m, a, b;
			n = (inst[0] >> 16) & 0xFF;
			m = (inst[0] >> 24) & 0xFF;
			a = inst[1] & 0xFFFF;
			b = inst[1] >> 16;
			VK_LOG ("%08X: Clear Unknown B [%08X %08X] %u %u <%X %X>",
			        gpu->pc, inst[0], inst[1], n, m, a, b);
			gpu->pc += 8;
		}
		break;

	/* Viewport */

	case 0x811:
		/* 811	Viewport: Unknown
		 *
		 *	aaaa aaaa aaaa aaaa ---- oooo oooo oooo
		 *	cccc cccc cccc cccc bbbb bbbb bbbb bbbb */
		{
			vec3s_t unk;

			unk.x[0] = inst[0] >> 16;
			unk.x[1] = inst[1] & 0xFFFF;
			unk.x[2] = inst[1] >> 16;

			VK_LOG ("%08X: Viewport: Unknown 811 [ unk = <%u %u %u> ]",
			        gpu->pc, unk.x[0], unk.x[1], unk.x[2]);

			VK_ASSERT (!(inst[0] & 0x0000F000));

			gpu->vp_scratch._811_params.unk = unk;
			gpu->pc += 8;
		}
		break;
	case 0x991:
		/* 991	Vireport: Unknown
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	---- ---s nnnn nnnn mmmm mmmm pppp pppp		s = Sign; n, m, p = Unknown
		 *
		 * See PH:@0C016368, PH:@0C016396 */
		{
			_991_params_t params;

			params.sign	= (inst[1] >> 24) & 1;
			params.unk.x[0]	= (inst[1] >> 16) & 0xFF;
			params.unk.x[1]	= (inst[1] >> 8) & 0xFF;
			params.unk.x[2]	= inst[1] & 0xFF;

			VK_LOG ("%08X: Viewport: Unknown 991 [ sign=%u unk=<%u %u %u> ]",
			        gpu->pc, params.sign, params.unk.x[0], params.unk.x[1], params.unk.x[2]);

			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			VK_ASSERT (!(inst[1] & 0xFE000000));

			gpu->vp_scratch._991_params = params;
			gpu->pc += 8;
		}
		break;
	case 0x021:
		/* 021	Set Viewport Projection
		 *
		 * 	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *      pppp pppp pppp pppp pppp pppp pppp pppp		p = alpha * cotf (angle / 2)
		 *      qqqq qqqq qqqq qqqq qqqq qqqq qqqq qqqq		q =  beta * cotf (angle / 2)
		 *      zzzz zzzz zzzz zzzz zzzz zzzz zzzz zzzz		z = Depth component, float
		 *
		 * See PH:@0C01587C, PH:@0C0158A4, PH:@0C0158E8 */
		{
			_021_params_t params;

			params.persp_x	= *(float *) &inst[1];
			params.persp_y	= *(float *) &inst[2];
			params.unk	= *(float *) &inst[3];

			VK_LOG ("%08X: Viewport: Set Projection [ px=%f py=%f unk=%f ]",
			        gpu->pc, params.persp_x, params.persp_y, params.unk);

			VK_ASSERT (!(inst[0] & 0xFFFFF000));

			gpu->vp_scratch._021_params = params;
			gpu->pc += 16;
		}
		break;
	case 0x221:
		/* 221	Set Viewport Extents
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	jjjj jjjj jjjj jjjj cccc cccc cccc cccc		c = X center; j = Y center
		 *	--YY YYYY YYYY YYYY -XXX XXXX XXXX XXXX		Y, X = Coord maximum; Y can be at most 512, X can be at most 640
		 *	--yy yyyy yyyy yyyy -xxx xxxx xxxx xxxx		y, x = Coord minimums; at least one of them MUST be zero
		 *
		 * See PH:@0C015924 */
		{
			_221_params_t params;

			params.center.x[0]	= inst[1] & 0xFFFF;
			params.center.x[1]	= inst[1] >> 16;
			params.extents_x.x[0]	= inst[2] & 0x7FFF;
			params.extents_x.x[1]	= inst[3] & 0x7FFF;
			params.extents_y.x[0]	= (inst[2] >> 16) & 0x3FFF;
			params.extents_y.x[1]	= (inst[3] >> 16) & 0x3FFF;

			VK_LOG ("%08X: Viewport: Set Extents [ center=<%u,%u> x=<%u,%u> y=<%u,%u> ]",
			        gpu->pc,
			        params.center.x[0], params.center.x[1],
			        params.extents_x.x[0], params.extents_x.x[1],
			        params.extents_y.x[0], params.extents_y.x[1]);

			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			VK_ASSERT (!(inst[2] & 0xC0008000));
			VK_ASSERT (!(inst[3] & 0xC0008000));

			gpu->vp_scratch._221_params = params;
			gpu->pc += 16;
		}
		break;
	case 0x421:
		/* 421	Set Depth
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx		x = Unknown
		 *	yyyy yyyy yyyy yyyy yyyy yyyy yyyy yyyy		y = Unknown
		 *	aaa- ---- ---- ---- ---- ---- ---- ----		a = Unknown
		 *
		 * See PH:@0C015AA6 */
		{
			_421_params_t params;

			params.depth_near	= *(float *) &inst[1];
			params.depth_far	= *(float *) &inst[2];
			params.depth_func	= inst[3] >> 29;

			VK_LOG ("%08X: Viewport: Set Depth [ near=%f far=%f func=%u ]",
			        gpu->pc, params.depth_near, params.depth_far, params.depth_func);
			        
			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			VK_ASSERT (!(inst[3] & 0x1FFFFFFF));

			gpu->vp_scratch._421_params = params;
			gpu->pc += 16;
		}
		break;
	case 0x621:
		/* 621	Set Shade Model
		 *
		 *	---- ---- ---- nnDb ---- oooo oooo oooo		n = Unknown, D = disable?, u = Unknown, o = Opcode
		 *      RRRR RRRR GGGG GGGG BBBB BBBB AAAA AAAA		RGBA = light color
		 *	ffff ffff ffff ffff ffff ffff ffff ffff		f = 1.0f OR 1.0f / (max - min) OR 1.0f / sqrt ((max - min)**2)
		 *	gggg gggg gggg gggg gggg gggg gggg gggg		g = kappa / max
		 *
		 * See PH:@0C0159C4, PH:@0C015A02, PH:@0C015A3E */
		{
			_621_params_t params;

			params.unk_n	 = (inst[0] >> 18) & 3;
			params.enabled	 = (inst[0] >> 17) & 1 ? 0 : 1;
			params.unk_b	 = (inst[0] >> 16) & 1;
			params.color	 = *(vec4b_t *) &inst[1];
			params.inv_delta = *(float *) &inst[2];
			params.inv_max	 = *(float *) &inst[3];

			VK_LOG ("%08X: Viewport: Set Shade Model [ enabled=%u n=%u b=%u color=<%X %X %X %X> inv_delta=%f inv_max=%f ]",
			        gpu->pc,
			        params.enabled, params.unk_n, params.unk_b,
			        params.color.x[0], params.color.x[1], params.color.x[2], params.color.x[3],
			        params.inv_delta, params.inv_max);

			VK_ASSERT (!(inst[0] & 0xFFF0F000));

			gpu->vp_scratch._621_params = params;
			gpu->pc += 16;
		}
		break;
	case 0x004:
		/* 004	Commit Viewport
		 * 104	Unknown
		 * 404	Unknown
		 * 504	Unknown
		 *
		 *	---- ---- ---- -nnn ---- oooo oooo oooo		o = Opcode. n = Unknown; n can't be zero
		 *
		 * See PH:@0C015AD0 */
		{
			unsigned n = (inst[0] >> 16) & 7;

			VK_LOG ("%08X: Commit Viewport [%08X] %u",
			        gpu->pc, inst[0], n);

			VK_ASSERT (!(inst[0] & 0xFFF8F000));

			gpu->vp[n] = gpu->vp_scratch;
			gpu->pc += 4;
		}
		break;
	case 0x003:
		/* 003	Recall Viewport
		 * 903	Unknown
		 * D03	Unknown
		 *
		 *	---- ---- ---- --nn -pq- oooo oooo oooo		o = Opcode. n = Unknown, p,q = Modifiers; if p 4 then n is ignored?
		 *
		 * See PH:@0C015AF6, PH:@0C015B12, PH:@0C015B32 */
		{
			unsigned n = (inst[0] >> 16) & 3;
			unsigned p = (inst[0] >> 14) & 1;
			unsigned q = (inst[0] >> 13) & 1;

			VK_LOG ("%08X: Recall Viewport [%08X] <%u %u %u>",
			        gpu->pc, inst[0], n, p, q);

			VK_ASSERT (!(inst[0] & 0xFFFC9000));

			gpu->pc += 4;
		}
		break;

	/* Color Operations */

	case 0x081:
	case 0x381:
		/* 081	Set Y Property 0
		 *
		 *	---- ---- ---- mmmm ---n oooo oooo oooo */
		{
			unsigned n, m;
			n = (inst[0] >> 12) & 1;
			m = (inst[0] >> 16) & 0xF;
			VK_LOG ("%08X: Color: Set Y 0 [%08X] %u %u",
			        gpu->pc, inst[0], n, m);
			gpu->pc += 4;
		}
		break;
	case 0x881:
		/* 881	Set Y Property 8 */
		VK_LOG ("%08X: Color: Set Y 8 [%08X]", gpu->pc, inst[0]);
		gpu->pc += 4;
		break;
	case 0xA81:
		/* 881	Set Y Property A */
		VK_LOG ("%08X: Color: Set Y A [%08X]", gpu->pc, inst[0]);
		gpu->pc += 4;
		break;
	case 0xC81:
		/* 881	Set Y Property C */
		VK_LOG ("%08X: Color: Set Y C [%08X]", gpu->pc, inst[0]);
		gpu->pc += 4;

		break;

	case 0x091:
		/* 091	Set Color Property 0 */
		{
			unsigned x = (inst[0] >> 16) & 0xFF;
			unsigned a = inst[1] & 0xFF;
			unsigned b = (inst[1] >> 8) & 0xFF;
			unsigned c = (inst[1] >> 16) & 0xFF;
			unsigned d = (inst[1] >> 24) & 0xFF;
			VK_LOG ("%08X: Color: Set 0 [%08X %08X] %u <%u %u %u %u>",
			        gpu->pc, inst[0], inst[1], x, a, b, c, d);
			gpu->pc += 8;
		}
		break;
	case 0x291:
		/* 291	Set Color Property 2 */
		{
			unsigned x = (inst[0] >> 16) & 0xFF;
			unsigned a = inst[1] & 0xFF;
			unsigned b = (inst[1] >> 8) & 0xFF;
			unsigned c = (inst[1] >> 16) & 0xFF;
			unsigned d = (inst[1] >> 24) & 0xFF;
			VK_LOG ("%08X: Color: Set 2 [%08X %08X] %u <%u %u %u %u>",
			        gpu->pc, inst[0], inst[1], x, a, b, c, d);
			gpu->pc += 8;
		}
		break;
	case 0x491:
		/* 491	Set Color Property 4 */
		{
			unsigned x = (inst[0] >> 16) & 0xFF;
			unsigned a = inst[1] & 0xFF;
			unsigned b = (inst[1] >> 8) & 0xFF;
			unsigned c = (inst[1] >> 16) & 0xFF;
			unsigned d = (inst[1] >> 24) & 0xFF;
			VK_LOG ("%08X: Color: Set 4 [%08X %08X] %u <%u %u %u %u>",
			        gpu->pc, inst[0], inst[1], x, a, b, c, d);
			gpu->pc += 8;
		}
		break;
	case 0x691:
		/* 691	Set Color Property 6
		 *
		 *	aaaa aaaa aaaa aaaa ---- oooo oooo oooo
		 *	cccc cccc cccc cccc bbbb bbbb bbbb bbbb */
		{
			unsigned a = inst[0] >> 16;
			unsigned b = inst[1] & 0xFFFF;
			unsigned c = inst[1] >> 16;
			VK_LOG ("%08X: Color: Set 6 [%08X %08X] <%u %u %u>",
			        gpu->pc, inst[0], inst[1], a, b, c);
			gpu->pc += 8;
		}
		break;
	case 0x084:
		/* 084	Commit Color 
		 *
		 *	---- ---- ---- nnnn ---m oooo oooo oooo		o = Opcode, n = Number
		 *
		 * See PH:@0C0153D4 */
		{
			unsigned n = (inst[0] >> 16) & 0xF;
			unsigned m = (inst[0] >> 12) & 1;
			VK_LOG ("%08X: Commit Color  [%08X] %u %u",
			        gpu->pc, inst[0], n, m);
			VK_ASSERT (!(inst[0] & 0xFFF0E000));
			gpu->pc += 4;
		}
		break;
	case 0x083:
		/* 083	Recall Color 
		 *
		 *	---- ---- nnnn nnnn ---m oooo oooo oooo		o = Opcode, m = Modifier, n = Unknown
		 *
		 * See @0C00657C */
		{
			unsigned n = (inst[0] >> 16) & 0xF;
			unsigned m = (inst[0] >> 12) & 1;
			VK_LOG ("%08X: Recall Color  [%08X] %u %u",
			        gpu->pc, inst[0], n, m);
			VK_ASSERT (!(inst[0] & 0xFF00E000));
			gpu->pc += 4;
		}
		break;

	/* Texture Params */

	case 0x0C1:
		/* 0C1	Set Tex Param 0
		 *
		 *	---- ---- mmmm nnnn ---- oooo oooo oooo		o = Opcode, n, m = Unknown
		 *
		 * See PH:@0C015B7A */
		VK_LOG ("%08X: Set Tex Param 0 [%08X]", gpu->pc, inst[0]);
		gpu->pc += 4;
		break;
	case 0x2C1:
		/* 2C1	Set Tex Param 2
		 *
		 *	nnnn nnnn mmmm mmmm pppp oooo oooo oooo		o = Opcode, n, m, p = Unknown */
		VK_LOG ("%08X: Set Tex Param 2 [%08X]", gpu->pc, inst[0]);
		gpu->pc += 4;
		break;
	case 0x4C1:
		/* 4C1	Set Tex Param 4
		 *
		 *	nnnn nnnn mmmm mmmm pppp oooo oooo oooo		o = Opcode, n, m, p = Unknown
		 *
		 * See PH:@0C015BA0 */
		VK_LOG ("%08X: Set Tex Param 4 [%08X]", gpu->pc, inst[0]);
		gpu->pc += 4;
		break;
	case 0x0C4:
		/* 0C4	Commit Tex Params
		 *
		 *	---- ---- ---- nnnn ---m oooo oooo oooo		o = Opcode, m = Unknown, n = Number */
		{
			unsigned n = (inst[0] >> 16) & 0xF;
			unsigned m = (inst[0] >> 12) & 1;
			VK_LOG ("%08X: Commit Tex Params [%08X] num=%u %u",
			        gpu->pc, inst[0], n, m);
			VK_ASSERT (!(inst[0] & 0xFFFEE000));
			gpu->pc += 4;
		}
		break;
	case 0x0C3:
		/* 0C3	Recall Tex Params
		 *
		 *	---- ---- ---- nnnn ---m oooo oooo oooo		o = Opcode, m = Unknown, n = Number */
		{
			unsigned n = (inst[0] >> 16) & 0xF;
			unsigned m = (inst[0] >> 12) & 1;
			VK_LOG ("%08X: Recall Tex Params [%08X] num=%u %u",
			        gpu->pc, inst[0], n, m);
			VK_ASSERT (!(inst[0] & 0xFF00E000));
			gpu->pc += 4;
		}
		break;

	/* Matrix Data */

	case 0x261:
		/* 261	Set Matrix Vector */
	case 0x961:
		/* 961	Set Matrix Vector */
	case 0xB61:
		/* B61	Set Matrix Vector */
	case 0x161:
		/* 161	Set Matrix Vector
		 *
		 *	---- ---- ---- mmii -nnn oooo oooo oooo		o = Opcode, e = Index in Matrix, n = Unknown
		 *	xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx		x = Component X, float
		 *	yyyy yyyy yyyy yyyy yyyy yyyy yyyy yyyy		y = Component Y, float
		 *	zzzz zzzz zzzz zzzz zzzz zzzz zzzz zzzz		z = Component Z, float
		 *
		 * See @0C008080
		 */
		{
			unsigned n = (inst[0] >> 12) & 7;
			unsigned m = (inst[0] >> 16) & 3;
			unsigned i = (inst[0] >> 18) & 3;
			vec3f_t *v = (vec3f_t *) &inst[1];

			VK_LOG ("%08X: Set Matrix Vector [%08X %08X %08X %08X] %u %u %u <%f %f %f>",
			        gpu->pc,
			        inst[0], inst[1], inst[2], inst[3],
			        n, m, i,
			        v->x[0], v->x[1], v->x[2]);

			gpu->pc += 16;
		}
		break;

	/* Vertex Data */

	case 0xEE8:
	case 0xEE9:
		/* EE8	Tex Coord 3 */
		/* EE9	Tex Coord 3 */
		{
			vec2s_t *uv0 = (vec2s_t *) &inst[1];
			vec2s_t *uv1 = (vec2s_t *) &inst[2];
			vec2s_t *uv2 = (vec2s_t *) &inst[3];

			VK_LOG ("%08X: Tex Coord <%u %u> <%u %u> <%u %u>",
			        gpu->pc,
			        uv0->x[0], uv0->x[1],
			        uv1->x[0], uv2->x[1],
			        uv1->x[0], uv2->x[1]);

			draw_tri (gpu, uv0, uv1, uv2);
			gpu->pc += 16;
		}
		break;
	case 0x1AC:
	case 0x1AD:
	case 0xFAC:
	case 0xFAD:
		/* xAC	Vertex 3f
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx		x = X coord
		 *	yyyy yyyy yyyy yyyy yyyy yyyy yyyy yyyy		y = Y coord
		 *	zzzz zzzz zzzz zzzz zzzz zzzz zzzz zzzz		z = Z coord
		 *	*/
		{
			vec3f_t *v = (vec3f_t *) &inst[1];

			VK_LOG ("%08X: Vertex { %f %f %f }",
			        gpu->pc,
			        v->x[0], v->x[1], v->x[2]);

			append_vertex (gpu, v);
			/*print_vertex_buffer (gpu);*/
			gpu->pc += 16;
		}
		break;
	case 0x1B8:
	case 0x1BC:
	case 0x1BD:
	case 0xFB8:
	case 0xFBC:
	case 0xFBD:
	case 0xFBE:
	case 0xFBF:
		/* 1BC	Vertex Normal 3f */
		{
			unsigned p = inst[0] >> 24;
			unsigned q = inst[4];
			vec3f_t *v = (vec3f_t *) &inst[1];
			vec3f_t *n = (vec3f_t *) &inst[5];
			VK_LOG ("%08X: Vertex Normal %u %u <%f %f %f> <%f %f %f>",
			        gpu->pc, p, q,
			        v->x[0], v->x[1], v->x[2],
			        n->x[0], n->x[1], n->x[2]);
			gpu->pc += 32;
		}
		break;

	case 0xE88:
		/* E88	Unknown [Flush Vertices?] */
		{
			VK_LOG ("%08X: Unknown %03X [%08X]",
			        gpu->pc, inst[0] & 0xFFF, inst[0]);
			gpu->pc += 4;
		}
		break;

	/* Unknown */

	case 0x101:
		/* 101	Unknown [Begin Scene]
		 *
		 * A	---- --uu uuuu uuuu ---- oooo oooo oooo		o = Opcode, u = Unknown
		 * B	---- ---- ---- -1mm nnnn oooo oooo oooo		o = Opcode, n,m = Unknown, XXX not so sure about this
		 *
		 * See @0C008040, PH:@0C016418, PH:@0C016446 */
		{
			unsigned u = (inst[0] >> 24) & 1;
			VK_LOG ("%08X: Unknown 101 [%08X] %u",
			        gpu->pc, inst[0], u);
			VK_ASSERT (!(inst[0] & 0xFEFFF000));
			gpu->pc += 4;
		}
		break;
	case 0x301:
		/* 301	Unknown */
		{
			VK_LOG ("%08X: Unknown 301 [%08X]", gpu->pc, inst[0]);
			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			gpu->pc += 4;
		}
		break;
	case 0x501:
		/* 501	Unknown */
		{
			VK_LOG ("%08X: Unknown 501 [%08X]", gpu->pc, inst[0]);
			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			gpu->pc += 4;
		}
		break;
	case 0x043:
		/* 043	Unknown [Reset All Color Entries?] */
		{
			unsigned u = (inst[0] >> 24) & 0xF;
			VK_LOG ("%08X: Recall Unknown 043 [%08X] %u",
			        gpu->pc, inst[0], u);
			VK_ASSERT (!(inst[0] & 0xF0FFF000));
			gpu->pc += 4;
		}
		break;
	case 0x901:
		/* 901	Unknown
		 *
		 *	---- ---- -nnn nnnn ---- oooo oooo oooo		o = Opcode, n = Unknown
		 * */
		{
			unsigned n = (inst[0] >> 16) & 0x7F;
			VK_LOG ("%08X: Unknown 901 [%08X] %u",
			        gpu->pc, inst[0], n);
			VK_ASSERT (!(inst[0] & 0xFF80F000));
			gpu->pc += 4;
		}
		break;
	case 0x3A1:
		/* 3A1	Set Lo Addresses; always comes in a pair with 5A1
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	llll llll llll llll llll llll llll llll
		 *	LLLL LLLL LLLL LLLL LLLL LLLL LLLL LLLL
		 *      0000 0000 0000 0000 0000 0000 0000 0000
		 *
		 * See PH:@0C016308 */
		{
			//uint32_t l1 = inst[1];
			//uint32_t l2 = inst[2];
			VK_LOG ("%08X: Set Lo Addresses [%08X %08X %08X %08X]",
			        gpu->pc, inst[0], inst[1], inst[2], inst[3]);
			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			VK_ASSERT (!inst[3]);
			gpu->pc += 16;
		}
		break;
	case 0x5A1:
		/* 5A1	Set Hi Addresses; always comes in a pair with 3A1
		 *
		 *	---- ---- ---- ---- ---- oooo oooo oooo		o = Opcode
		 *	uuuu uuuu uuuu uuuu uuuu uuuu uuuu uuuu
		 *	UUUU UUUU UUUU UUUU UUUU UUUU UUUU UUUU
		 *      0000 0000 0000 0000 0000 0000 0000 0000
		 *
		 * See PH:@0C016308 */
		{
			//uint32_t u1 = inst[1];
			//uint32_t u2 = inst[2];
			VK_LOG ("%08X: Set Hi Addresses [%08X %08X %08X %08X]",
			        gpu->pc, inst[0], inst[1], inst[2], inst[3]);
			VK_ASSERT (!(inst[0] & 0xFFFFF000));
			VK_ASSERT (!inst[3]);
			gpu->pc += 16;
		}
		break;
	case 0x6D1:
		/* 6D1	Unknown
		 *
		 *	aaaa aaaa aaaa aaaa ---- oooo oooo oooo		o = Opcode
		 *	bbbb bbbb bbbb bbbb cccc cccc cccc cccc
		 *
		 * See PH:@0C015C3E */
		{
			unsigned a = inst[0] >> 16;
			unsigned b = inst[1] & 0xFFFF;
			unsigned c = inst[1] >> 16;
			VK_LOG ("%08X: Unknown 6D1 [%08X %08X] <%u %u %u>",
			        gpu->pc, inst[0], inst[1], a, b, c);
			VK_ASSERT (!(inst[0] & 0x0000F000));
			gpu->pc += 8;
		}
		break;
	case 0x181:
		/* 181	Unknown
		 *
		 *	---- ---b nnnn nnnn ---- oooo oooo oooo		o = Opcode, n = Unknown, b = set if n > 0 (rather n != 0)
		 *
		 * See PH:@0C015B50 */
		{
			unsigned b = (inst[0] >> 24) & 1;
			unsigned n = (inst[0] >> 16) & 0xFF;
			VK_LOG ("%08X: Unknown 181 [%08X] <%u %u>",
			        gpu->pc, inst[0], b, n);
			VK_ASSERT (!(inst[0] & 0xFE00F000));
			gpu->pc += 4;
		}
		break;

	case 0x303:
		/* 303	Unknown
		 *
		 *	uuuu ---- ---- ---- ---- oooo oooo oooo		o = Opcode, u = Unknown */
		{
			unsigned u = inst[0] >> 24;
			VK_LOG ("%08X: Unknown 303 [%08X] %u",
			        gpu->pc, inst[0], u);
			VK_ASSERT (!(inst[0] & 0x00FFF000));
			gpu->pc += 4;
		}
		break;
	case 0x104:
		/* 104	Unknown */
		{
			unsigned n = (inst[0] >> 16) & 7;
			VK_LOG ("%08X: Commit Matrix [%08X] %u",
			        gpu->pc, inst[0], n);
			gpu->pc += 4;
		}
		break;
	case 0x051:
		/* 051	Unknown Vertex-related */
		{
			vec4b_t *unk = (vec4b_t *) &inst[1];
			VK_LOG ("%08X: Vertex: Unknown [%08X %08X] <%u %u %u %u>",
			        gpu->pc, inst[0], inst[1],
			        unk->x[0], unk->x[1], unk->x[2], unk->x[3]);
			gpu->pc += 8;
		}
		break;
	case 0x006:
		/* 006	Unknown */
	case 0x046:
		/* 046	Unknown */
		VK_LOG ("%08X: Unknown %03X [%08X]",
		        gpu->pc, inst[0] & 0xFFF, inst[0]);
		gpu->pc += 4;
		break;
	case 0x451:
		/* 451	Unknown */
		VK_LOG ("%08X: Unknown %03X [%08X %08X]",
		        gpu->pc, inst[0] & 0xFFF, inst[0], inst[1]);
		gpu->pc += 8;
		break;
	case 0x064:
		/* 064	Unknown */
	case 0x561:
		/* 561	Unknown */
		VK_LOG ("%08X: Unknown %03X [%08X %08X %08X %08X]",
		        gpu->pc, inst[0] & 0xFFF, inst[0], inst[1], inst[2], inst[3]);
		gpu->pc += 16;
		break;

#if 0
	case 0x12C:
	case 0x12D:
	case 0x72C:
	case 0x72D:
		/* x2C	Unknown 3f */
		/* x2D	Unknown 3f */
		{
			vec3f_t *v = &inst[1];
			gpu->pc += 16;
		}
		break;
	case 0x158:
	case 0x159:
	case 0xF58:
	case 0xF59:
		/* 158	Unknown Vertex-related */
		{
			vec2s_t *unk = &inst[1];
			gpu->pc += 8;
		}
		break;
	case 0x711:
		/* 711	Unknown
		 *
		 *	aaaa aaaa aaaa aaaa ---- oooo oooo oooo
		 *	bbbb bbbb bbbb bbbb cccc cccc cccc cccc
		 *
		 * See PH:@0C0162E2 */
		{
			unsigned a = inst[0] >> 16;
			unsigned b = inst[1] & 0xFFFF;
			unsigned c = inst[1] >> 16;
			gpu->pc += 8;
		}
		break;
#endif
	default:
		VK_ABORT ("GPU: unhandled opcode %03X", inst[0] & 0xFFF);
	}
	return 0;
}

/*
 * GPU Execution
 * =============
 *
 * Very few things are known. Here are my guesses:
 *
 * GPU execution is likely initiated by:
 *  15000058 = 3
 *  1A000024 = 1
 *
 * A new frame subroutine is uploaded when both IRQs 2 of GPU 15 and GPU 1A
 * are fired, meaning that they both consumed the data passed in and require
 * new data (subroutine) to continue processing.
 *
 * When execution ends:
 * CHECK ALL THIS AGAIN PLEASE
 *  1A00000C bit 0 is set
 *  1A000018 bit 1 is set as a consequence
 *  1A000018 bit 1 is set 
 *  15000088 bit 7 is set as a consequence
 *  15000088 bit 1 is set; a GPU IRQ is raised (if not masked by 15000084)
 *  15002000 bit 0 is set on some HW revisions
 *  1A000024 bit 0 is cleared if some additional condition occurred
 *
 * 15002000 and 1A000024 signal different things; see the termination
 * condition in sync_for_frame ()
 */

static void
hikaru_gpu_begin_processing (hikaru_gpu_t *gpu)
{
	/* Check the GPU 15 execute bits */
	if (REG15 (0x58) == 3) {
		gpu->is_running = true;

		gpu->pc = REG15 (0x70);
		gpu->sp[0] = REG15 (0x74);
		gpu->sp[1] = REG15 (0x78);

		memset (gpu->vertex_buffer, 0, 3 * sizeof (vec3f_t));
		gpu->vertex_index = 0;
	}
}

static void
hikaru_gpu_end_processing (hikaru_gpu_t *gpu)
{
	/* Turn off the busy bits */
	REG15 (0x58) &= ~3;
	REG1A (0x24) &= ~1;

	/* Notify that GPU 15 is done and needs feeding */
	hikaru_gpu_raise_irq (gpu, _15_IRQ_DONE, _1A_IRQ_DONE);
}

/* FIXME check 15:10 and 15:14 role in IDMA and IRQ generation; this is
 * actually broken */

static int
hikaru_gpu_exec (vk_device_t *dev, int cycles)
{
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) dev;

	/* Step the GPU 15 indirect DMA thing */
	if (REG15 (0x14) & 1)
		hikaru_gpu_step_idma (gpu);

	/* Step the GPU 1A texture FIFO thing */
	/* TODO */

	if (!gpu->is_running || REG15 (0x58) != 3)
		return 0;

	hikaru_gpu_print_state (gpu);

	/* XXX hack, no idea how fast the GPU is or how much time each
	 * command takes. */
	gpu->cycles = cycles;
	while (gpu->cycles > 0) {
		if (hikaru_gpu_exec_one (gpu)) {
			hikaru_gpu_end_processing (gpu);
			gpu->cycles = 0;
			break;
		}
		gpu->cycles--;
	}

	return 0;
}

void
hikaru_gpu_vblank_in (vk_device_t *dev)
{
	(void) dev;
}

void
hikaru_gpu_vblank_out (vk_device_t *dev)
{
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) dev;
	hikaru_gpu_raise_irq (gpu, _15_IRQ_VBLANK, _1A_IRQ_VBLANK);
}

/*
 * FIFO at 1A040000
 * ================
 *
 * Transfers textures to TEXRAM?
 *
 * See PH:@0C0CD320, SN-ROM:@0C070C9C
 *
 * 1A040000  32-bit  W	Source Address?
 * 1A040004  32-bit  W  Destination Address?
 * 1A040008  32-bit  W	Texture size as a pair of 16-bit values
 * 1A04000C  32-bit  W	FIFO Control
 *
 * Access to this thing is illegal (likely) while 1A000024 bit 0 is set.
 *
 * FIFO busy is signalled in 1A000024 bit 0. When operation ends, and IRQ is
 * raised? Unlikely, unless programmed?
 */

static void
hikaru_gpu_begin_fifo_operation (hikaru_gpu_t *gpu)
{
	uint32_t *fifo = (uint32_t *) gpu->regs_1A_fifo;

	VK_ASSERT (!(REG1A (0x24) & 1));

	VK_LOG ("GPU 1A FIFO exec: %08X ---> %08X, %u x %u, control = %08X",
		fifo[0] * 2, fifo[1] * 2,
		(fifo[3] & 0xFFFF) / 2, fifo[3] >> 16,
		fifo[4]);

	gpu->fifo_is_running = true;

	REG1A (0x24) |= 1; /* XXX where does this come from? */

	VK_ASSERT (0);
}

static int
hikaru_gpu_get (vk_device_t *dev, unsigned size, uint32_t addr, void *val)
{
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) dev;
	uint32_t *val32 = (uint32_t *) val;

	VK_ASSERT (size == 4);

	*val32 = 0;
	if (addr >= 0x15000000 && addr < 0x15000100) {
		switch (addr & 0xFF) {
		case 0x10:
		case 0x88:
			break;
		case 0x14:
			/* XXX hack, indirect DMA is always done */
			REG15 (0x14) &= ~1;
			break;
		default:
			return -1;
		}
		*val32 = REG15 (addr);
	} else if (addr == 0x15002000) {
		/* no-op */
	} else if (addr == 0x18001000) {
		/* SEGA PCI ID */
		*val32 = 0x17C711DB;
	} else if (addr >= 0x1A000000 && addr < 0x1A000140) {
		switch (addr & 0x1FF) {
		case 0x18: /* GPU 1A IRQ Status */
			*val32 = (REG1A (0x08) & 1) |
			         ((REG1A (0x0C) & 1) << 1) |
			         ((REG1A (0x10) & 1) << 2) |
			         ((REG1A (0x14) & 1) << 3);
			break;
		case 0x1C:
		case 0x20: /* XXX ^= 1 */
		case 0x24: /* XXX = 2 */
		case 0x100: /* Tex UNITs busy/ready */
			break;
		default:
			return -1;
		}
		*val32 = REG1A (addr);
	} else if (addr >= 0x1A000180 && addr < 0x1A0001C0) {
		*val32 = REG1AUNIT (0, addr);
	} else if (addr >= 0x1A000200 && addr < 0x1A000240) {
		*val32 = REG1AUNIT (1, addr);
	} else if (addr >= 0x1A080000 && addr < 0x1A080100) {
		/* ??? */
	}
	return 0;
}

static int
hikaru_gpu_put (vk_device_t *device, size_t size, uint32_t addr, uint64_t val)
{
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) device;

	VK_ASSERT (size == 4);

	if (addr >= 0x15000000 && addr < 0x15000100) {
		switch (addr & 0xFF) {
		case 0x00:
		case 0x04:
		case 0x08:
		case 0x0C:
		case 0x10:
		case 0x14:
		case 0x18 ... 0x34:
		case 0x38 ... 0x54:
		case 0x70 ... 0x78:
		case 0x80:
		case 0x8C:
		case 0x90:
		case 0x94:
		case 0x98:
			break;
		case 0x58: /* Control */
			REG15 (0x58) = val;
			hikaru_gpu_begin_processing (gpu);
			return 0;
		case 0x84: /* IRQ mask */
			REG15 (addr) = val;
			hikaru_gpu_update_irqs (gpu);
			return 0;
		case 0x88: /* IRQ status */
			REG15 (addr) &= val;
			hikaru_gpu_update_irqs (gpu);
			return 0;
		default:
			return -1;
		}
		REG15 (addr) = val;
	} else if (addr == 0x1502C100) {
		VK_ASSERT (val == 9);
	} else if (addr == 0x1502C104) {
		VK_ASSERT (val == 6);
	} else if (addr == 0x15040E00) {
		VK_ASSERT (val == 0);
	} else if (addr >= 0x18001000 && addr < 0x18001020) {
		REG18 (addr) = val;
	} else if (addr >= 0x1A000000 && addr < 0x1A000104) {
		switch (addr & 0x1FF) {
		case 0x00:
		case 0x04:
			break;
		case 0x08: /* GPU 1A IRQ 0 */
		case 0x0C: /* GPU 1A IRQ 1 */
		case 0x10: /* GPU 1A IRQ 2 */
		case 0x14: /* GPU 1A IRQ 3 */
			/* Bit 0 is ANDNOT'ed on write; I have no clue about
			 * the other bits. */
			VK_ASSERT (val == 1);
			REG1A(addr) &= ~val;
			hikaru_gpu_update_irqs (gpu);
			return 0;
		case 0x24:
			REG1A (addr) = val;
			hikaru_gpu_begin_processing (gpu);
			return 0;
		case 0x80 ... 0xC0: /* Display Config? */
		case 0xC4: /* Unknown control */
		case 0xD0: /* Unknown control */
		case 0x100: /* Tex UNITs busy/ready */
			break;
		default:
			return -1;
		}
		REG1A (addr) = val;
	} else if (addr >= 0x1A000180 && addr < 0x1A0001C0) {
		REG1AUNIT (0, addr) = val;
	} else if (addr >= 0x1A000200 && addr < 0x1A000240) {
		REG1AUNIT (1, addr) = val;
	} else if (addr == 0x1A020000) {
		/* "SEGA" */
		VK_ASSERT (val == 0x53454741);
	} else if (addr >= 0x1A040000 && addr < 0x1A040010) {
		REG1AFIFO (addr) = val;
		if (addr == 0x1A04000C && (val & 1) == 1)
			hikaru_gpu_begin_fifo_operation (gpu);
	} else if (addr == 0x1A0A1600) {
		VK_ASSERT (val == 1);
	}
	return 0;
}

static void
hikaru_gpu_reset (vk_device_t *dev, vk_reset_type_t type)
{
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) dev;

	memset (gpu->regs_15, 0, 0x100);
	memset (gpu->regs_18, 0, 0x100);
	memset (gpu->regs_1A, 0, 0x104);
	memset (gpu->regs_1A_unit[0], 0, 0x40);
	memset (gpu->regs_1A_unit[0], 0, 0x40);
	memset (gpu->regs_1A_fifo, 0, 0x10);
}

static int
hikaru_gpu_save_state (vk_device_t *dev, FILE *fp)
{
	return -1;
}

static int
hikaru_gpu_load_state (vk_device_t *dev, FILE *fp)
{
	return -1;
}

static void
hikaru_gpu_delete (vk_device_t **_dev)
{
	FREE (_dev);
}

vk_device_t *
hikaru_gpu_new (vk_machine_t *mach, vk_buffer_t *cmdram, vk_buffer_t *texram)
{
	hikaru_gpu_t *gpu = ALLOC (hikaru_gpu_t);
	if (gpu) {
		gpu->base.mach = mach;

		gpu->base.delete	= hikaru_gpu_delete;
		gpu->base.reset		= hikaru_gpu_reset;
		gpu->base.exec		= hikaru_gpu_exec;
		gpu->base.get		= hikaru_gpu_get;
		gpu->base.put		= hikaru_gpu_put;
		gpu->base.save_state	= hikaru_gpu_save_state;
		gpu->base.load_state	= hikaru_gpu_load_state;

		gpu->cmdram = cmdram;
		gpu->texram = texram;
	}
	return (vk_device_t *) gpu;
}
