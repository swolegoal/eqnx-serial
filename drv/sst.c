/* Equinox SST driver for Linux.
 *
 * Copyright (C) 1999-2006 Equinox Systems Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#define STATIC
#include <linux/config.h>
#include <linux/version.h>
// #if	(LINUX_VERSION_CODE < 132608)
// /* 2.2 and 2.4 kernels */
// #define __NO_VERSION__
// #endif

#ifdef CONFIG_MODVERSIONS
#define MODVERSIONS	1
#endif

// #if	(LINUX_VERSION_CODE < 132608)
// /* 2.2 and 2.4 kernels */
// #ifdef MODVERSIONS
// #include <linux/modversions.h>
// #endif /* MODVERSIONS */
// #endif

#include <linux/module.h>
#define EQNX_VERSION_CODE LINUX_VERSION_CODE
#include <linux/errno.h>

#ifdef  MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif  /* MODULE_LICENSE */                                                

#if	(LINUX_VERSION_CODE >= 132608)
/* 2.6+ kernels */
#include <linux/vermagic.h>
#include <linux/compiler.h>
MODULE_INFO(vermagic, VERMAGIC_STRING);
static const struct modversion_info ____versions[]
__attribute__((section("__versions"))) = {
};

#endif

/* Correct compile problems on Mandrake 6.? and SuSE 7.0 */
#ifdef __SMP__
#ifndef CONFIG_X86_LOCAL_APIC
#define CONFIG_X86_LOCAL_APIC
#endif
#endif
                
#include <linux/sched.h> 
#include <linux/timer.h>
#include <linux/wait.h>

#include <linux/fcntl.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/string.h>

#if	(LINUX_VERSION_CODE >= 132096)
/* 2.4 kernels and after */
#include <linux/list.h>
#include <linux/isapnp.h>
#endif

#include <asm/io.h>
#if (LINUX_VERSION_CODE >= 132096)
#include <linux/spinlock.h>
#else
#include <asm/spinlock.h>
#endif

#if (EQNX_VERSION_CODE > 131327)
#include <asm/uaccess.h>  /* 2.1.x+ kernel changes */
#endif

#if	(LINUX_VERSION_CODE >= 132608)
/* 2.6 kernels and after */
#include <linux/pci.h>
#endif

/*
 * Define hardware specific data types
 * needed by this driver.
 */
#include "icp.h"
#include "eqnx.h"
#include "brdtab.h"
#include "eqnx_ioctl.h"
#include "ist.h"
#include "ramp.h"

#if (EQNX_VERSION_CODE < 131328)  /* kernel 2.0.x and below */
#define ioremap vremap
#define iounmap vfree
#define eqn_to_user memcpy_tofs
#define eqn_from_user memcpy_fromfs
#define eqn_fatal_signal (current->signal & ~current->blocked)

#else				/* kernel 2.1.x and above */
#define put_fs_int put_user
#define eqn_to_user copy_to_user
#define eqn_from_user copy_from_user
#define eqn_fatal_signal (signal_pending(current))
#endif

/*
** Maximum number of boards, may be redefined
*/
#define	MAXBOARD	4
int maxbrd = MAXBOARD;
int din_num[ (MAXBOARD / 2) ];
int dout_num[ (MAXBOARD / 2) ];
int diag_num;

#ifdef	ISA_ENAB
/* A value in MegAddr will force all ISA boards to use a 16K memory 
   window at this address. Generally, it is better to let the dynamic
   configuration pick addresses. This is here to allow an override in
   cases where the dynamic configuration fails. 
   The boards will still be set as 16 bit paged mode*/

unsigned long int MegAddr = 0x0;	/* force ISA memory hole if non-zero */
#endif	/* ISA_ENAB */

/* RAMP START --------------------------------------------- */
#define DTOC(index, d) { \
unsigned char d0[8]; \
int dtoc_j, dtoc_i; \
for(dtoc_j = 0, dtoc_i = 10000000; dtoc_i ; dtoc_j++, dtoc_i /= 10) { \
   if (d < dtoc_i) \
      d0[dtoc_j] = 0; \
   else { \
      d0[dtoc_j] = d / dtoc_i; \
      d /= 10; \
   } \
} \
dtoc_j = 0; \
while(d0[dtoc_j] == 0) dtoc_j++; \
}
   
#define WTOC(index, w) \
HTOC(index,(w & 0xFF00) >> 8); \
HTOC(index,(w & 0xFF));

#define LTOC(index,l) \
WTOC(index,(l & 0xFFFF0000) >> 16); \
WTOC(index,(l & 0xFFFF));

#ifdef	IA64
/*
** required due to problems with memcpy on IA64
*/
void memcpy_IA64(void *, void *, int);
#define	memcpy(a,b,c)	memcpy_IA64(a,b,c)
#endif	/* IA64 */

/* Tests if the channel is on a RAMP */
#define ISRAMP(mpc) \
( mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id == 0x08 || \
mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id == 0x09 || \
mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id == 0x0B )

/* Returns the slot structure given the mpc */
#define GET_SLOT(mpc) \
(&(slot_dev[mpc - meg_chan]))

/* Returns modem type. Valid only if slot_state = 0xFF. 00 = ISA.  01 = PNP. */
#define MODEM_TYPE(mpc) \
( GET_SLOT(mpc)->type )

/* Returns modem slot state. */
#define SLOT_STATE(mpc) \
( GET_SLOT(mpc)->slot_state )

/* Returns 1 if power on, 0 if power off. */
#define POWER_STATE(mpc) \
((mpc->mpc_icp->slot_ctrl_blk + ((mpc->mpc_chan) % 64))->power_state == 0xFF)

/* Tests if any call back flags are set in the state parameter */
#define MPA_CALL_BACK(state)  \
   ((state &  MPA_INITG_UART) | \
   (state & MPA_INITG_SET_LOOP_BACK) | \
   (state & MPA_INITG_CLR_LOOP_BACK) | \
   (state & MPA_INITG_START_BREAK) | \
   (state & MPA_INITG_STOP_BREAK) | \
   (state & MPA_INITG_INIT_MODEM) | \
   (state & MPA_INITG_MODIFY_SETTINGS) | \
   (state & MPA_INITG_HARD_RESET))

/* Sets up mpd, slot_chan, port, lmx, icp, curmarb given the mpc */
#define PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb) \
   mpd = mpc->mpc_mpd; \
   slot_chan = (mpc->mpc_chan) % 64; /*0 - 63, channel relative to SSP*/ \
   port = slot_chan % 16; /* 0 - 15, channel relative to LMX */ \
   lmx = slot_chan / 16; /* 0 - 3, lmx relative to SSP */ \
   icp = mpc->mpc_icp; \
   curmarb = GET_MARB(mpc);

/* Returns the address of the MARB given the mpc */
#define GET_MARB(mpc) \
(&(((mpc->mpc_icp)->slot_ctrl_blk + ((mpc->mpc_chan) % 64))->marb))

/* returns the saved control signals given the mpc */
#define GET_SAVED_CTRL_SIGS(mpc) \
(((GET_SLOT(mpc))->sv_cout_cntrl_sig & 0x02BB) | 0x0004)

/* Sets parameter val to control signals given mpc */
#define GET_CTRL_SIGS(mpc,val) \
val = mpc->mpc_cout->ssp.cout_ctrl_sigs; \
if (ISRAMP(mpc)) { \
   unsigned int state = (MPA_INITG_SET_LOOP_BACK | MPA_INITG_CLR_LOOP_BACK); \
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~(state))) { \
      if (mpc->mpc_mpa_cout_ctrl_sigs) \
         val = mpc->mpc_mpa_cout_ctrl_sigs; \
      else \
         val = GET_SAVED_CTRL_SIGS(mpc); \
   } \
}

/* sets control signals to val parameter given mpc */
#define SET_CTRL_SIGS(mpc, val) \
if (ISRAMP(mpc)) { \
   unsigned int set_sigs_state = (MPA_INITG_SET_LOOP_BACK | MPA_INITG_CLR_LOOP_BACK); \
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~set_sigs_state)) \
      mpc->mpc_mpa_cout_ctrl_sigs = val; \
   else \
      (mpc->mpc_icpo)->ssp.cout_ctrl_sigs = val; \
} else \
   (mpc->mpc_icpo)->ssp.cout_ctrl_sigs = val;

/* Prints or saves formatted RAMP message */
#define MESSAGE(module, retv, mpc) { \
unsigned int m_slot_chan = (mpc->mpc_chan) % 64; \
unsigned int m_port = (m_slot_chan % 16) + 1; \
unsigned int m_lmx = (m_slot_chan / 16) + 1; \
unsigned int m_board = mpc->mpc_brdno + 1; \
if (eqn_ramp_trace) \
{ \
printk("%s 0x%.4X board(%d) port(%d) lmx(%d)\n",module,retv,m_board,m_port,m_lmx); \
}\
}

/* 
 * Prints RAMP formatted message. Also saves it if verbose messages turned 
 * off. 
 */
#define PRINT(module, retv, mpc) { \
int p_save = eqn_ramp_trace; \
MESSAGE(module, retv, mpc); \
eqn_ramp_trace = p_save; \
/* If saving other messages, save these too. */ \
if (!eqn_ramp_trace) \
MESSAGE(module, retv, mpc); \
}
/* RAMP END ----------------------------------------------- */

struct sys_functs_struct ramp_import_blk = {0};

struct ist_struct ist; /* Installation Status Table */

int register_eqnx_dev(void);
static int megastty( struct mpchan *mpc, struct tty_struct *tp, 
		int arg, int dev);
#if (EQNX_VERSION_CODE > 131327)
static ssize_t eqnx_diagread(struct file *fp, char *buf, size_t count, loff_t *fpos);
#else
static int eqnx_diagread(struct inode *ip, struct file *fp, char *buf, 
		int count);
#endif
STATIC int eqnx_diagopen(struct inode *ip, struct file *fp);
STATIC int eqnx_diagclose(struct inode *ip, struct file *fp);
STATIC int eqnx_diagioctl(struct inode *ip, struct file *fp, unsigned int cmd,
            unsigned long arg);
int move_brd_addr( unsigned long int addr);
#ifndef MODULE
static void eqnx_meminit(long base);
static long eqnx_memhalt(void);
#endif
static void *eqnx_memalloc(int len);
static int eqnx_setserial(struct mpchan *mpc, struct serial_struct *sp);
static int set_modem_info(struct mpchan *mpc, unsigned int cmd,
		unsigned int *value, struct tty_struct *);
static void eqnx_getserial(struct mpchan *mpc, struct serial_struct *sp);
static int eqnx_setserial(struct mpchan *mpc, struct serial_struct *sp);
static uchar_t eqn_cs_get( struct mpchan *mpc);
static void megatxint(struct mpchan *mpc);
static void megainput( register struct mpchan *mpc, unsigned long);
static void megamint(register struct mpchan *mpc);
static void megasint(register struct mpchan *mpc, unsigned long);
STATIC int eqnx_ioctl(struct tty_struct *tty, struct file * file,
            unsigned int cmd, unsigned long arg);
static void sst_write( struct mpchan *mpc, 
		unsigned char *buf, int count);
static void sst_write1(struct mpchan *mpc, int func_type);
static void eqnx_flush_chars(struct tty_struct *tty);
static void eqnx_flush_chars_locked(struct tty_struct *tty);
static int get_modem_info(struct mpchan *mpc, unsigned int *value);
static int mega_push_winicp( struct mpdev *mpd, int icp);
static void mega_rdv_wait( unsigned long arg);
static void mega_ldv_hangup( icpaddr_t icp);
static void mega_rdv_delta( icpaddr_t icp, struct mpchan *mpc, int rng_state);
static int mega_rng_delta( icpaddr_t icp, int reason, int nchan);
static void eqnx_dohangup(void *arg);
static void eqnx_flush_buffer(struct tty_struct *tty);
static void eqnx_flush_buffer_locked(struct tty_struct *tty);
static void eqnx_delay(int len);
static int megajam( register struct mpchan *mpc, char c);
static int frame_wait( register struct mpchan *mpc, int count);
static int frame_ctr_reliable( icpgaddr_t icpg, register struct mpchan *mpc);
static int mega_pop_win(int instance);
static int mega_push_win( struct mpchan *mpc, int typ);
static int megaparam( int d);
static int megamodem(int d, int cmd);
static  ushort_t icpbaud( int val, struct mpchan *mpc);
static int chanon( struct mpchan *mpc);
static int chanoff( struct mpchan *mpc);
static int cur_chnl_sync( register struct mpchan *mpc);
#ifdef	MCA_ENAB
static uint_t mca_mem_strat( uchar_t *regs );
static uint_t mca_mem_size( uchar_t *regs, int req );
static paddr_t mca_base_paddr( uchar_t *regs );
static int mca_brd_found( int slot, uchar_t *regs);
#endif	/* MCA_ENAB */
static uint_t pci_mem_size( struct brdtab_t *);
static unsigned long int ramp_block_int(void);
static void ramp_unblock_int(unsigned long int flags);
void ramp_fsm( struct mpchan *mpc);
void *ramp_map_fn( void *mpc);
void ramp_unmap_fn(void *mpc);
void ramp_hard_reset( struct mpchan *mpc);
void ramp_hard_reset_error( struct mpchan *mpc, int retv);
void ramp_modem_cleanup( struct mpchan *mpc);
void ramp_init_modem_error( struct mpchan *mpc, int retv);
void ramp_set_loop_back_error( struct mpchan *mpc, int retv);
void ramp_clr_loop_back_error( struct mpchan *mpc, int retv);
int ramp_clr_loop_back( struct mpchan *mpc);
int ramp_set_loop_back( struct mpchan *mpc);
int ramp_start_break( struct mpchan *mpc);
void ramp_start_break_error( struct mpchan *mpc, int retv);
void ramp_stop_break_error( struct mpchan *mpc, int retv);
void ramp_stop_break( struct mpchan *mpc);
void ramp_reg_modem_error( struct mpchan *mpc, int retv);
void ramp_dereg_modem_cleanup( struct mpchan *mpc);
void ramp_dereg_modem( struct mpchan *mpc);
int ramp_get_delay( struct mpchan *mpc);
static void eqnrflush( struct mpchan *mpc);
void ramp_reg_modem( struct mpchan *mpc);
void ramp_check_error( struct mpchan *mpc, int retv);
static int ramp_id_functs(void);
static void ramparam( struct mpchan *mpc);
int ramp_get_index( struct mpchan *mpc);
void ramp_set_index( struct mpchan *mpc);
void ramp_set_initg( struct mpchan *mpc, int index);
void ramp_init_modem( struct mpchan *mpc);
static ushort_t uart_baud( int val, struct mpchan *mpc);
static int SSTMINOR_FROMDEV(unsigned int dev);
static int SSTMINOR(unsigned int maj, unsigned int min);

unsigned char inc_tail (volatile struct icp_in_struct *);
unsigned char mpa_fn_access_sspcb_lst (unsigned long int *);
void mpa_fn_relse_sspcb_lst (unsigned long int);

void brd_mem_cfg( struct mpdev *mpd );
int eqx_pci_buspresent(struct pci_csh *csh);

static int mem_zero( struct mpdev *mpd, unsigned short *ramw, 
		int testlen, int icp);
static int mem_test(struct mpdev *mpd, int icp);
int mem_test_dram( struct mpdev *mpd, ushort_t *ramw, int testlen, int icp );
int mem_test_pram( struct mpdev *mpd, ushort_t *ramw, int testlen, int icp );
int mem_test_tag(struct mpdev *mpd, uchar_t *ramb, int testlen, int icp);
int test_8bit_hole(struct mpdev *mpd, int k, paddr_t addr );

int write_wakeup_deferred = 0;

#ifdef	ISA_ENAB
static int eisa_brd_found( int slot, ushort_t *regs );
static paddr_t eisa_base_paddr( ushort_t *regs );
static uint_t eisa_mem_size( ushort_t *regs, int req );
static uint_t eisa_mem_strat( ushort_t *regs );
int eisa_brdtyp_ok( ushort_t bbbbbrrr, ushort_t cfg );
static unsigned char determ_isa_cnfg( unsigned short int , 
         struct isa_cnfg_parms *, unsigned char);

struct operator_parms  oper_input = {
								/* USER INPUTS */
	(unsigned char)		( 0 ) ,				/*System's Bus Type					*/
	(unsigned char)		( 0 ) ,				/*Default baud rate table				*/
	(unsigned char)		( 1 ) ,				/*Quiet Mode flag					*/
	(unsigned char)		( 0 ) ,				/*Number of Host Controllers				*/

	(unsigned short int)	( MAX_ISA_ARRAY_SIZE ) ,	 /*Number of valid elements in ISA Hole Table		*/
	  			{				/*Table of 16k memory hole Physical Addresses to check	*/
								/*NOTE: Table arranged from most probable memory hole	*/
								/*	location to least probable.			*/

								/*ARRAY INDEX (decimal)					*/
					0xD0000,		/*     00						*/
					0xB4000,		/*     01						*/

					0xB0000,		/*     02						*/
					0xD4000,		/*     03						*/
					0xD8000,		/*     04						*/
					0xDC000,		/*     05						*/

					0xE0000,		/*     06						*/
					0xE4000,		/*     07						*/
					0xE8000,		/*     08						*/
					0xEC000,		/*     09						*/

					0xC0000,		/*     10						*/
					0xC4000,		/*     11						*/
					0xC8000,		/*     12						*/
								        
					0xB8000,		/*     13						*/
					0xBC000,		/*     14						*/

					0xCC000,		/*     15						*/

					0xA0000,		/*     16						*/
					0xA4000,		/*     17						*/
					0xA8000,		/*     18						*/
					0xAC000,		/*     19						*/
				      } ,			/*End of Memory Hole Table				*/

	(unsigned char)		( 0 ) ,				/*Bit 31 Memory Check flag				*/
	(unsigned int)		( MAX_BIT_31_ARRAY_SIZE-1 ) ,	/*Number valid elements in bit 31 table			*/
				{				/*Table of 16k mem hole Physc Addrs w/bit 31 set	*/
				      /*NOTE: 0x80000000 can not be used since appears to shadow low 1M of memory	*/

								/*ARRAY INDEX (decimal)					*/
					0x80100000,		/*     00						*/
					0x80200000,		/*     01						*/
					0x80300000,		/*     02						*/
					0x80400000,		/*     03						*/
					0x80500000,		/*     04						*/
					0x80600000,		/*     05						*/
					0x80700000,		/*     06						*/
					0x80800000,		/*     07						*/
					0x80900000,		/*     08						*/
					0x80A00000,		/*     09						*/
					0x80B00000,		/*     10						*/
					0x80C00000,		/*     11						*/
					0x80D00000,		/*     12						*/
					0x80E00000,		/*     13						*/
					0x80F00000,		/*     14						*/
				      } ,			/*End of bit 31 memory hole array			*/

								/* USER INPUTS */
/*RV7*/	(unsigned char)		( 1 ) ,				/*VGA 8-bit memory Override 				*/
	(unsigned char)		( 0 ) ,				/*Default Controller parameter spec'd			*/
				{ 0 } ,				/*Default Controller parameters				*/
        (unsigned char)		( 0 ) ,				/*Number of entries in explicit Configuration table	*/
             			{{ 0x200, 0x80104000l, 0, 0xFF }}	/*Table entry						*/
	} ;  /* oper_input */
#endif
/* ISA_ENAB */

static uchar_t fifo_mask [] = { 0x0, 0x3, 0xf, 0x3f, 0xff, 0x3, 0xf, 0x3f, 0xff};

uint_t icpbaud_tbl[] = { 0, 50, 75, 110, 134, 150, 200, 300, 
                           600, 1200, 1800, 2400, 4800, 9600, 19200, 38400,
				57600, 115200,230400, 460800, 921600};

#define NCHAN		64		/* usable channels per SSP64 */
#define NCHAN_BRD	128		/* max channels per board */
#ifndef MIN
#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))
#endif
#define MPDIALIN	0x1000		/* dial-in line */
#define MPSHARED	0x2000		/* uses channel pairing */
#define MPNOMODEM	0x4000		/* modem lines not used */
#define MPSCRN		0x8000		/* multi screen */

#define EQNX_RAW	0
#define EQNX_COOKED	1
#define EQNX_TXINT	2

#define TURNOFF		0		/* turn this channel off */
#define TURNON		1		/* turn this channel on */

#define LOCK_A		1		/* Bank A lock bit */
#define LOCK_B		2		/* Bank B lock bit */

#define MPTIMEO		((HZ*10)/1000)	/* 10ms. polling interval */

#define CLSTIMEO	0		/* no timeout */
#define EQNX_CLOSEDELAY ((HZ*500)/1000)	/* 1/2 second */

#define false		0
#define true		(!false)	
#define	HWREGSLEN	0x4000
#define HWRXQWRAP	3
#define HWTXQWRAP	0x0b

#define EISA_EQX        0x3816
#define EISA_REGS_LEN	9
#define	EISA_MEM_LEN	0x200000
#define DSQSIZE		4096 	/*0x1000*/
#define DSQMASK		4096-1  /*0x0fff*/
/* Note: chan is set and cleared via the EQNCHMONSET/CLR ioctls */

static struct {
	int open;		/* true/false: Is the the data monitor open */
	int chan;		/* port number being scoped, meg_chan index*/
	int status;		/* status (for WRAP) */
#define DSQWRAP	0x01
	queue_t q;		/* Manage circular q buffer */
	int next;		/* queue structure does not have this */
	char buffer[DSQSIZE];	/* Actual circular buffer */

#if	(LINUX_VERSION_CODE < 132096)
	/* kernels before 2.4 */
	struct wait_queue *scope_wait;
#else
	/* 2.4 kernels and after */
	wait_queue_head_t scope_wait;
#endif
	int	scope_wait_wait;

} dscope[2];

/*****************************************************************************/

#if	(LINUX_VERSION_CODE < 132096)
	/* kernels before 2.4 */
static struct file_operations	eqnx_fdiag = {
	NULL,
	eqnx_diagread,
	NULL,
	NULL,
	NULL,
	eqnx_diagioctl,
	NULL,
	eqnx_diagopen,
#if (EQNX_VERSION_CODE > 131327)
	NULL,		
#endif	
	eqnx_diagclose,
	NULL
};
#elif	(LINUX_VERSION_CODE < 132608)
	/* 2.4 kernels */
static struct file_operations	eqnx_fdiag = {
	read:	eqnx_diagread,
	ioctl:	eqnx_diagioctl,
	open:	eqnx_diagopen,
	release: eqnx_diagclose,
};
#else
	/* 2.6 kernels */
static struct file_operations	eqnx_fdiag = {
	.read =		eqnx_diagread,
	.ioctl =	eqnx_diagioctl,
	.open =		eqnx_diagopen,
	.release = 	eqnx_diagclose,
	.owner =	THIS_MODULE, 
};
#endif

/*****************************************************************************/

#ifdef	MCA_ENAB
#define MCA_REGS_LEN	12		/* max bytes of info */
#define MCA_EQX_64	0x6388
#define MCA_EQX_128	0x6389
#define MCA_EQX_8	0x638a
static
ushort_t mca_iobase[] = { 0x3000, 0x3080, 0x3400, 0x3480,
                          0x3800, 0x3880, 0x3c00, 0x3c80,
                          0x7000, 0x7080, 0x7400, 0x7480,
                          0x7800, 0x7880, 0x7c00, 0x7c80,
                          0xb000, 0xb080, 0xb400, 0xb480,
                          0xb800, 0xb880, 0xbc00, 0xbc80,
                          0xf000, 0xf080, 0xf400, 0xf480,
                          0xf800, 0xf880, 0xfc00, 0xfc80 };
#endif	/* MCA_ENAB */

#define FLAT128_MEM_LEN	0x200000
#define FLAT64_MEM_LEN	0x100000
#define FLAT8_MEM_LEN	0x10000
#define FLAT16K_MEM_LEN	0x4000
#define FLAT32K_MEM_LEN	0x8000
#define FLAT64K_MEM_LEN	0x10000
#define	WIN16_MEM_LEN	0x4000


#define HA_FLAT 0
#define HA_WIN16	1

#define HWCMDQSIZE	0
#define CIN_DEF_ENABLES		0x3800
#define COUT_DEF_ENABLES	0x8000
#define VTIMEO		192
#ifndef uint_t
#define uint_t unsigned int
#endif

#define RNG_BAD         0
#define RNG_GOOD        1
#define RNG_CHK		2
#define RNG_WAIT	3
/* NOTE: RNG_FAIL (value 4) is also used in mega_rng_delta() */

#define DEV_VIRGIN      0xff            /* device id is unknown */
#define DEV_BAD         0               /* device was known but now offline */
#define DEV_GOOD        1               /* device known and online */
#define DEV_INSANE      2               /* hardware error (unexpected) */
#define DEV_WAITING	3
#define MAXLDVMPC	16
#define WINDO_DEPTH	128

#define LINK_OFF	0x80

#define MM_COUNTRY_CODE_REG 	0x8800

#define HWQ4SIZE        4096		/* size of queue - each chnl has two */
#define HWQ4BITS        0x0fff    	/* 12 bit mask for 4096 */
#define HWQ4RXWRAP      0x04		/* 4096 */
#define HWQ4TXWRAP      0x0c		/* 4096, circular */
#define HWQ4HIWAT       HWQ4SIZE * 7 / 8
#define HWQ4LOWAT       HWQ4SIZE / 8
#define HWQ4CMDSIZE     0x04    	/* 1024 = 4096/4 */
#define HWQ4MINCHAR	HWQ4SIZE / 2

#define HWQ2SIZE        2048		/* size of queue - each chnl has two */
#define HWQ2BITS        0x07ff    	/* 12 bit mask for 2048 */
#define HWQ2RXWRAP      0x03		/* 2048 */
#define HWQ2TXWRAP      0x0b		/* 2048, circular */
#define HWQ2HIWAT       HWQ2SIZE * 3 / 4
#define HWQ2LOWAT       HWQ2SIZE / 4
#define HWQ2CMDSIZE     0x03    	/* 512 = 2048/4 */
#define HWQ2MINCHAR	HWQ2SIZE / 2

/* for SSM */
#define HWQ1SIZE        1024            /* size of queue - each chnl has two */
#define HWQ1BITS        0x03ff          /* 12 bit mask for 1024 */
#define HWQ1RXWRAP      0x02            /* 1024 */
#define HWQ1TXWRAP      0x0a            /* 1024, circular */
#define HWQ1HIWAT       960             /* 1024 - 64 */
#define HWQ1LOWAT       HWQ1SIZE / 4
#define HWQ1CMDSIZE     0x03            /* 512 = 2048/4 */
#define HWQ1MINCHAR	64

#define HWQ_5SIZE        512           /* size of queue - each chnl has two */
#define HWQ_5BITS        0x01ff          /* 12 bit mask for 512 */
#define HWQ_5RXWRAP      0x01            /* 512 */
#define HWQ_5TXWRAP      0x09            /* 512, circular */
#define HWQ_5HIWAT       448            /* 512- 64 */
#define HWQ_5LOWAT       HWQ_5SIZE / 4
#define HWQ_5CMDSIZE     0x02            /* 256 = 2048/8 */
#define HWQ_5MINCHAR	 32
/* end SSM changes   */

/* begin RS422 changes */
#define RS422   1
#define LMX_8E_422         0x06           /* ss8-e 422 */
#define LMX_PM16_422       0x07           /* rm16-rj 422 */
/* end RS 422 changes */

#define HWREGSLEN	0x4000		/* 16k for every icp */

static struct hwq_struct sst_hwq[] =
{
  { HWQ4SIZE, HWQ4BITS, HWQ4HIWAT, HWQ4LOWAT,
    HWQ4RXWRAP, HWQ4TXWRAP, HWQ4CMDSIZE,HWQ4MINCHAR },
  { HWQ2SIZE, HWQ2BITS, HWQ2HIWAT, HWQ2LOWAT,
    HWQ2RXWRAP, HWQ2TXWRAP, HWQ2CMDSIZE,HWQ2MINCHAR }
  ,{ HWQ1SIZE, HWQ1BITS, HWQ1HIWAT, HWQ1LOWAT,
     HWQ1RXWRAP, HWQ1TXWRAP, HWQ1CMDSIZE,HWQ1MINCHAR },
  {  HWQ_5SIZE, HWQ_5BITS, HWQ_5HIWAT, HWQ_5LOWAT,
     HWQ_5RXWRAP, HWQ_5TXWRAP, HWQ_5CMDSIZE,HWQ_5MINCHAR }
};

/* It is possible to sleep in copyin/copyout due to 
   swapping and/or paging.  In order to ensure that
   multiple processes don't sleep and wake up in a
   different order and then use the incorrect window info
   from the stack, it is necessary to lock the window
   when doing the copyin/out calls.  In this way, only one
   process (context) can sleep with information on the 
   stack.  Other uses of the window stack (e.g., megapoll) 
   are ok.  They will push their window usage and pop them 
   correctly (even if interrupted by megapoll). By design,
   window information is not left on the stack when sleeping.
   However, the window must be active when doing copyin/out
   in bypass mode.  This locking mechanism ensures that only
   one process can be doing a copyin/out at a time. NOTE: this
   only applies to copyin/out requests that are moving data
   from the board.  Copies of data structures (between 
   kernel memory and user memory) are not affected.
   Monty 9/14/94  */

struct windo_stk
{
	ushort	pgctrl;
	ushort	pgdata;
        struct mpdev *ws_mpd;
};



unsigned int ramp_admin_poll;
int eqn_num_ramps = 0;

static int wtos = 0;		/* window stack index */
int wtos_max = 0;               /* to debug window code */
int last_pop = 1;		/* to debug window code */
int last_pop1 = 0;		/* to debug window code */
int last_pop2 = 0;		/* to debug window code */
int last_pop3 = 0;		/* to debug window code */
int last_pop4 = 0;		/* to debug window code */
int last_pop5 = 0;		/* to debug window code */
int last_pop6 = 0;		/* to debug window code */
int last_pop7 = 0;		/* to debug window code */

unsigned int poll_cnt = 0;	/* used to delay input processing in megapoll */
/* The following tables assign the transmit Q low water mark based on
   baud rate. It is based on (#chars per 40 milliseconds/64) plus some slop. 

   lowat baud rates = 0, 50, 75, 110, 134, 150, 200, 300, 
                           600, 1200, 1800, 2400, 4800, 9600, 19200, 38400 
				57600, 115200,230400, 460800, 921600

   The table contains the result. 1.07 */

uint_t lowat[] = { 0, 2, 2, 2, 2, 2, 2, 2, 
                           2, 2, 2, 2, 2, 2, 2, 4, 
				8, 14, 20, 40, 80};

/* The following table assigns the transmit Q low water mark based on
   baud rate. It is based on (#chars per 10 milliseconds/64) plus some 
   slop. */
uint_t ssp4_lowat[] = { 0, 2, 2, 2, 2, 2, 2, 2, 
                           2, 2, 2, 2, 2, 2, 2, 2, 
				2, 3, 5, 8, 12};

int eqn_ramp_trace = 0;	/* RAMP verbose messages flag */
int eqn_quiet_mode = 0;	/* suppress ring config messages when true */
int eqn_save_trace = 0; /* Remember ramp_trace flag when in quiet_mode */
struct cst_struct brd_cst[MAXBRDCHNL]; /* used in brdstatus ioctl */
static int nextmin;
static int nmegaport = 0;		/* number of boards found */
static int nicps;			/* number of icps found */
static int to_eqn_ramp_admin = -1;

#define IOBUSTYPE 1  			/* I/O bus type: 1=ISA,2=EISA,3=MCA */

#define MAX_BACKDOOR_PORTS 4

#ifdef	ISA_ENAB
extern unsigned long int MegAddr;
#endif	/* ISA_ENAB */

int dtr_down = 9; /* Drop DTR for 900 milliseconds when HUPCL */
int io_bus_type = IOBUSTYPE;
int EQNft = 0; /* For Star Technologies, make 1 for sentinel/mra */

struct mpdev meg_dev[NMEGAPORT]; /* Adapter struct */
struct ssp_struct ssp_dev[NMEGAPORT * NSSP4S]; /* sspcb struct */
struct slot_struct slot_dev[MAXCHNL];
struct mpchan *meg_chan;

static ushort_t portn[] = {0x200, 0x220, 0x240, 0x260, 0x280, 0x2a0, 0x2c0, 
		0x2e0, 0x300, 0x320, 0x340, 0x360, 0x380, 0x3a0, 0x3c0, 0x3e0};

struct windo_stk wstk[WINDO_DEPTH];
#ifdef	ISA_ENAB
static uchar_t cmn_irq[] = {0xff, 0x3, 0x4, 0x5, 0x7, 0xa, 0xb, 0xc};
#endif	/* ISA_ENAB */


/* serial subtype definitions */
#define SERIAL_TYPE_NORMAL	1
#define SERIAL_TYPE_CALLOUT	2
// #if	(LINUX_VERSION_CODE < 132608)
// /* 2.2 and 2.4 kernels */
// static int eqnx_refcount;
// #endif
#define pcisize  0x44			/* PCI data structure size */
char *eqnPCIcsh;
struct tty_driver *eqnx_driver; 
#if (LINUX_VERSION_CODE < 132096)
struct tty_driver *eqnx_callout_driver;
#endif
struct tty_struct **eqnx_ttys;
struct termios **eqnx_termios;
struct termios **eqnx_termioslocked;
static struct timer_list lmx_wait_timer;
static struct timer_list eqnx_timer, eqnx_ramp_timer;
void sstpoll(unsigned long arg);
void eqn_ramp_admin(unsigned long arg);

/*	Create temp write buffer for copying from user space to avoid
 	page fault problems.  */

static char	*eqnx_tmpwritebuf = (char *) NULL;

#if	(LINUX_VERSION_CODE < 132096)
	/* kernels before 2.4 */
static struct semaphore  eqnx_tmpwritesem = MUTEX;
#else
	/* 2.4 kernels and after */
static struct semaphore  eqnx_tmpwritesem;
#endif

static spinlock_t ramp_lock;
spinlock_t eqnx_mem_hole_lock;

/*	Local buffer for copying output characters. Used in eqnx_put_char */
static char	*eqnx_txcookbuf = (char *) NULL;
static int	eqnx_txcooksize = 0;
static int	eqnx_txcookrealsize = 0;
static struct tty_struct	*eqnx_txcooktty = (struct tty_struct *) NULL;

/*	Default termios structure.  */

static struct termios		eqnx_deftermios = {
	0,
	0,
	/*(B9600 | CS8 | CREAD | HUPCL),*/
	(B9600 | CS8 | CREAD | HUPCL | CLOCAL),
	0,
	0,
	INIT_C_CC
};

#define	XMIT_BUF_SIZE		4096

struct brdtab_t unknown_board = {
	NOID, NOID, NOBUS, 0, 1, 0, 0, "Unknown" };

#include "brdtab.c"

/*
 *	Memory allocation vars. These keep track of what memory allocation
 *	we can currently use. They help deal with memory in a consistent
 *	way, whether during init or run-time.
 */
static int	eqnx_meminited = 0;
static long	eqnx_memend;
#ifdef MODULE
int init_module(void);
void cleanup_module(void);

#if	(LINUX_VERSION_CODE >= 132608)
/* 2.6+ kernels */
MODULE_AUTHOR("Mike Straub");
MODULE_DESCRIPTION("Equinox SST Driver");
MODULE_LICENSE("GPL");
#endif

#endif

#if	(LINUX_VERSION_CODE >= 132096)
#ifdef	ISA_ENAB
/*
** ISA plug-and-play in linux kernels 2.4+
*/
#define	MAX_PNP_DEVS	4
struct	pci_dev	*pnp_found_devs[MAX_PNP_DEVS];
int	pnp_found = 0;
#endif
#endif

// #if	(LINUX_VERSION_CODE < 132608)
// /* 2.2 and 2.4 kernels */
// #define	MY_GROUP()	(current->pgrp)
// #else
/* 2.6 kernels and after */
#define	MY_GROUP()	(pid_nr(current->signal->tty->pgrp))
// #endif

/*
** eqnx_open(tty, filp)
**
** Channel open - called whenever any sst channel
** is opened by a process.  
*/
int eqnx_open(struct tty_struct *tty, struct file * filp)
{
	register struct mpchan *mpc;
	register struct tty_struct *tp;
	int d, minor, major;
	struct mpdev *mpd;
	int win16;
	int nchan;
	int rc = 0;
	unsigned long flags;

// #if	(LINUX_VERSION_CODE < 132608)
// 	/* 2.2 and 2.4 kernels */
// 	MOD_INC_USE_COUNT;
// 	minor = MINOR(tty->device);
// 	major = MAJOR(tty->device);
// #else
	/* 2.6+ kernels */
	try_module_get(THIS_MODULE);
	major = tty->driver->major;
	minor = tty->driver->minor_start + tty->index;
// #endif

	d = SSTMINOR(major, minor);

#ifdef DEBUG
	printk("eqnx_open:for device %d\n",d);
#endif
	if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
		|| (d < 0)) {
		return(-ENODEV);
	}

        if ( meg_dev[MPBOARD(d)].mpd_alive == 0){
		return(-ENODEV);
	}
        mpc = &meg_chan[d];
	if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
		return(-ENODEV);
	}

	/*
	** save major and minor numbers
	*/
	mpc->mpc_major = major;
	mpc->mpc_minor = minor;

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpd->mpd_lock, flags);

	/* Do not allow ports to open on SST-16 boards without a panel */
	if (mpd->mpd_board_def->flags & NOPANEL) {
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return(-ENODEV);
	}

	nchan = MPCHAN(d);
	if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
		nchan >= (int)mpd->mpd_nchan){
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return(-ENODEV);
	}

	/* Verify that the hardware for this port is in a good state */
	if (mpc->mpc_icp->icp_rng_state != RNG_GOOD){
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return(-ENODEV);
	}
	if (mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_active != DEV_GOOD){
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return(-ENODEV);
	}

	/* if a bridge, check the mux */
	if (mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_type & 1) 
		if (mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_rmt_active != DEV_GOOD){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			return(-ENODEV);
	}

	/* check for channel outside valid range, ie
           PM8      (0-7)
           ssm-12X  (0-11)
           ssm-24X  (0-11, 16-27)
	*/
	if (mpd->mpd_board_def->asic == SSP64)  /* only applies to SSP64 */
            if ( ( MPCHAN(d) % 16) >= mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_chan)
            {
#ifdef	DEBUG
                 printk("eqnx_open: mpchan(%d) A(%d) lmx_no(%d) lmx_chan(%d)\n",
                   MPCHAN(d),MPCHAN(d)%16,mpc->mpc_lmxno,
                      mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_chan);
#endif	/* DEBUG */
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return(-ENODEV);
            }

	win16 = mpd->mpd_nwin;
	/* RAMP START --------------------------------------------- */
	/* If RAMP config not completed, don't let any sends in */
	if (ISRAMP(mpc)) {
		icpiaddr_t icpi;
		icpbaddr_t icpb;
	/* check break bit */
	/* If not receiving a break, the slot is occupied by a UART */
      
		if ( !(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE)){
      			icpi = mpc->mpc_icpi;
			if(win16)
				mega_push_win(mpc, 0);
      			icpb=(rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b 
				: &icpi->ssp.cin_bank_a;
  			if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  				frame_wait(mpc,2); /* make sure regs are valid*/ 
         		if (!(icpb->ssp.bank_sigs & 0x01 )) {
				if(win16) 
					mega_pop_win(80);
            			/* Modem present in slot, sleep */
modem_wait:
				mpc->mpc_mpa_call_back_wait++;
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
// #if	(LINUX_VERSION_CODE < 132608)
// 				/* 2.2 and 2.4 kernels */
// 				interruptible_sleep_on(&mpc->mpc_mpa_call_back);
// #else
				/* 2.6 kernels */
				wait_event_interruptible(mpc->mpc_mpa_call_back,
					mpc->mpc_mpa_call_back_wait == 0);
// #endif
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				if eqn_fatal_signal {
					spin_unlock_irqrestore(&mpd->mpd_lock, flags);
					return(-ERESTARTSYS);
				}
				if(mpc->mpc_mpa_stat & MPA_CALL_BACK_ERROR){
					spin_unlock_irqrestore(&mpd->mpd_lock, flags);
                        		return(-ENXIO);
				}
				else if ( !(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE)){
					goto modem_wait;
				}
         		} else {
				if(win16) 
					mega_pop_win(81);
            			/* No modem, return error */
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#ifdef DEBUG
				printk("eqnx_open: No modem at device %d\n", d);
#endif
				return(-ENODEV);
         		}
		}
	}
	/* RAMP END ----------------------------------------------- */

	mpc->refcount++;

	if(win16)
		mega_push_win(mpc, 0);

	if (!(mpc->flags & ASYNC_INITIALIZED)) {
		mpc->mpc_tty = tty;
		tty->driver_data = mpc;
// #if	(LINUX_VERSION_CODE < 132608)
// 		/* 2.2 and 2.4 kernels */
// 		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
// 			*tty->termios = *mpc->normaltermios;
// #if (LINUX_VERSION_CODE < 132096)
// 		/* 2.2 kernels */
// 		else
// 			*tty->termios = *mpc->callouttermios;
// #endif
// #else
		/* 2.6 kernels */
		*tty->termios = *mpc->normaltermios;
// #endif
#ifdef RS422
		/*    force CLOCAL on for RS422 ports */
		if (mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id == LMX_8E_422 ||
			mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id == 
			LMX_PM16_422) {
			tp = mpc->mpc_tty;
			tp->termios->c_cflag |= CLOCAL;
		}
#endif	/* RS422 */

		if((mpc->mpc_flags & MPC_OPEN) == 0) {
#ifdef DEBUG
			printk("eqnx_open:for device %d and flags = %x, calling megaparam with flags != ASYNC_INITIALIZED\n", d, (unsigned int) filp->f_flags);
#endif
			chanon(mpc);
			megaparam(d);
		}
		mpc->carr_state = megamodem(d, TURNON); /*shashi 02/02/98*/
		mpc->flags |= ASYNC_INITIALIZED;
		clear_bit(TTY_IO_ERROR, &tty->flags);
	}
	tp = mpc->mpc_tty;

	if (tp->termios == NULL) {
// #if     (LINUX_VERSION_CODE < 132608)
// 		/* 2.2 and 2.4 kernels */
// 		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
// 			*tp->termios = *mpc->normaltermios;
// #if (LINUX_VERSION_CODE < 132096)
// 		/* 2.2 kernels */
// 		else
// 			*tp->termios = *mpc->callouttermios;
// #endif
// #else
		/* 2.6 kernels */
		*tp->termios = *mpc->normaltermios;
// #endif
	}

/*
 *	If port is in the middle of closing, then wait. Get error status
 *	from flag settings.
 */
	if (mpc->flags & ASYNC_CLOSING) {
#ifdef DEBUG
		printk("eqnx_open:sleeping up on close_wait for device %d\n",d);
#endif
		if(win16) 
			mega_pop_win(81);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
// #if	(LINUX_VERSION_CODE < 132608)
// 		/* 2.2 and 2.4 kernels */
// 		interruptible_sleep_on(&mpc->close_wait);
// #else
		/* 2.6 kernels */
		wait_event_interruptible(mpc->close_wait, 
			(mpc->flags & ASYNC_CLOSING) == 0);
// #endif
		if (mpc->flags & ASYNC_HUP_NOTIFY)
			return(-EAGAIN);

		return(-ERESTARTSYS);
	}

#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	if (tp->driver.subtype == SERIAL_TYPE_CALLOUT){

#ifdef DEBUG
		printk("opening dialout device %d with flags %x\n", d, (unsigned int) filp->f_flags);
#endif
		if (mpc->flags & ASYNC_NORMAL_ACTIVE){
			if(win16) 
				mega_pop_win(81);
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			return(-EBUSY);
		}
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		if (mpc->flags & ASYNC_CALLOUT_ACTIVE){
			if((mpc->flags & ASYNC_SESSION_LOCKOUT) &&
				(mpc->session != current->session)){
				if(win16) 
					mega_pop_win(81);
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
				return(-EBUSY);
			}
			if((mpc->flags & ASYNC_PGRP_LOCKOUT) &&
				(mpc->pgrp != MY_GROUP())){
				if(win16) 
					mega_pop_win(81);
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
				return(-EBUSY);
			}
		}
		mpc->flags |= ASYNC_CALLOUT_ACTIVE;
#endif
	}
	else{
#endif

#ifdef DEBUG
		printk("opening dialin device %d with flags %o\n", d, (unsigned int) filp->f_flags);
#endif
		if (filp->f_flags & O_NONBLOCK) {
#if	(LINUX_VERSION_CODE < 132608)
			/* 2.2 and 2.4 kernels */
			if (mpc->flags & ASYNC_CALLOUT_ACTIVE){
				if(win16) 
					mega_pop_win(81);
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
				return(-EBUSY);
			}
#endif

#ifdef DEBUG
			printk("opening dialin device %d non_blocking with c_flags = %o\n", d,
		tp->termios->c_cflag);
#endif
		} else {
			mpc->openwaitcnt++;
			if (mpc->refcount)
				mpc->refcount--;
			while (1) {
#if	(LINUX_VERSION_CODE < 132608)
				/* 2.2 and 2.4 kernels */
				if (!(mpc->flags & ASYNC_CALLOUT_ACTIVE)){
					mpc->carr_state = megamodem(d, TURNON); /*shashi 02/02/98*/
				}
#else
					mpc->carr_state = megamodem(d, TURNON); /*shashi 02/02/98*/
#endif

				if (tty_hung_up_p(filp) || ((mpc->flags & ASYNC_INITIALIZED) == 0)) {
					if (mpc->flags & ASYNC_HUP_NOTIFY)
						rc = -EBUSY;
					else
						rc = -ERESTARTSYS;
					break;
				}
				if (((mpc->flags & ASYNC_CLOSING) == 0) &&
				    ((tp->termios->c_cflag & CLOCAL) || 
				     (mpc->carr_state)) &&

#if	(LINUX_VERSION_CODE < 132608)
				/* 2.2 and 2.4 kernels */
				     ((mpc->flags & ASYNC_CALLOUT_ACTIVE) == 0)) {
#else
				/* 2.6 kernels */
				     (1)) {
#endif

#ifdef DEBUG
	printk("device %d open returning with c_cflag = %o, carr_state = %d\n", d, tp->termios->c_cflag, mpc->carr_state);
#endif
					break;
				}
				if eqn_fatal_signal {
					rc = -ERESTARTSYS;
					break;
				}
#ifdef DEBUG
	printk("device %d going to sleep on open_wait\n", d);
#endif
				/*
				 * Wait if we're a dial-in line and DCD is low
				 */
				mpc->open_wait_wait++;
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
				/* 2.2 and 2.4 kernels */
				interruptible_sleep_on(&mpc->open_wait);
#else
				/* 2.6 kernels */
				wait_event_interruptible(mpc->open_wait, 
					mpc->open_wait_wait == 0);
#endif
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				if eqn_fatal_signal {
					rc = -ERESTARTSYS;
					break;
				}
				if (mpc->carr_state)
					break;
			}
			if(!tty_hung_up_p(filp))
				mpc->refcount++;
			mpc->openwaitcnt--;
			if (rc < 0){
				if(win16)
	   				mega_pop_win(1);
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#ifdef DEBUG
	printk("device %d returning with mpc_flags = %x, c_cflag = %o, carr_state = %d, rc = %d\n", d, mpc->flags, tp->termios->c_cflag, mpc->carr_state, rc);
#endif
				return(rc);
			}
		}
#ifdef DEBUG
		printk("device %d returning from open with mpc_flags = %x, c_cflag = %o, carr_state = %d, rc = %d\n", d, mpc->flags, tp->termios->c_cflag, mpc->carr_state, rc);
#endif
		mpc->flags |= ASYNC_NORMAL_ACTIVE;
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	}
#endif
	if ((mpc->refcount == 1) && (mpc->flags & ASYNC_SPLIT_TERMIOS)) {
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
		if (tp->driver.subtype == SERIAL_TYPE_NORMAL)
			*tp->termios = *mpc->normaltermios;
#if (LINUX_VERSION_CODE < 132096)
		else
			*tp->termios = *mpc->callouttermios;
#endif
#else
	/* 2.6 kernels and later */
	*tp->termios = *mpc->normaltermios;
#endif

	}
	if (mpd->mpd_board_def->asic != SSP64)  /* only applies to SSP2/SSP4 */
		if (!mpc->xmit_buf){
			unsigned long page;
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
			/* 2.2 and 2.4 kernels */
			page = get_free_page(GFP_KERNEL);
#else
			/* 2.6+ kernels */
			page = get_zeroed_page(GFP_KERNEL);
#endif
			if (!page){
				if(win16) 
					mega_pop_win(81);
				return(-ENOMEM);
			}
			spin_lock_irqsave(&mpd->mpd_lock, flags);
			mpc->xmit_buf = (unsigned char *)page;
			mpc->xmit_head = 0;
		}

#if	(LINUX_VERSION_CODE < 132608)
	mpc->session = current->session;
#endif
	mpc->pgrp = MY_GROUP();

	if(win16)
	   mega_pop_win(2);
      
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
	
	return(0);
}

/*
** eqnx_close(tty, filp)
**
** Channel close called on last close of any device.
** Release resources, reset device, and wakeup any sleepers
** waiting for channel to become idle.
*/
static void eqnx_close(struct tty_struct * tty, struct file * filp)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	int nchan;
	unsigned long flags;
	int d;
	int win16;

	if (mpc == (struct mpchan *) NULL)
		return;
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return;
#ifdef DEBUG
	printk("close:device %d being closed with flags = %x\n", d, mpc->flags);
#endif
	if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL)
		return;
	nchan = MPCHAN(d);
	if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
		nchan >= (int)mpd->mpd_nchan)
		return;
	win16 = mpc->mpc_mpd->mpd_nwin;
#ifdef DEBUG
	printk("device %d being closedwith refcount = %d\n", d, mpc->refcount);
#endif
	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	if (tty_hung_up_p(filp)){
	/* 2.6 kernels */
		module_put(THIS_MODULE);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return;
	}
	if (mpc->refcount)
		mpc->refcount -= 1;
	if (mpc->refcount) {
	/* 2.6 kernels */
		module_put(THIS_MODULE);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#ifdef DEBUG
		printk("device %d being closed with refcount %d \n", 
				d, mpc->refcount);
		printk("TTY_FLIPBUF_SIZE = %d and flip.count = %d for device %d with refcount\n",
				TTY_FLIPBUF_SIZE, tty->flip.count, d);
#endif
		return;
	}

	mpc->flags |= ASYNC_CLOSING;
#ifdef DEBUG
	printk("device %d being closed with flags = %x\n", d, mpc->flags);
#endif

	if (mpc->flags & ASYNC_NORMAL_ACTIVE)
		*mpc->normaltermios = *tty->termios;
#if (LINUX_VERSION_CODE < 132096)
	if (mpc->flags & ASYNC_CALLOUT_ACTIVE)
		*mpc->callouttermios = *tty->termios;
#endif
	if (tty == eqnx_txcooktty) {
		eqnx_flush_chars_locked(tty);
	}
	tty->closing = 1;

	if (mpc->mpc_flags & MPC_BUSY) 
		if (mpc->closing_wait != ASYNC_CLOSING_WAIT_NONE){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			tty_wait_until_sent(tty, 
					(unsigned long) mpc->closing_wait);
			spin_lock_irqsave(&mpd->mpd_lock, flags);
		}

	mpc->flags &= ~ASYNC_INITIALIZED;
#ifdef DEBUG
	printk("TTY_FLIPBUF_SIZE = %d and flip.count = %d for device %d\n",
		TTY_FLIPBUF_SIZE, tty->flip.count, d);
#endif
	if(win16) 
	      mega_push_win(mpc, 0);
	if (tty->termios->c_cflag & HUPCL ) {
		mpc->flags &= ~ASYNC_INITIALIZED;
		(void) megamodem(d, TURNOFF);
	}
#ifdef DEBUG
	printk("after HUPCL for device %d\n",d);
#endif
	mpc->mpc_flags &= ~(MPC_SOFTCAR|MPC_DIALOUT|MPC_MODEM|MPC_DIALIN|MPC_CTS);
	mpc->mpc_scrn_no = 0;
	mpc->mpc_icpi->ssp.cin_attn_ena &= ~ENA_DCD_CNG;
	mpc->mpc_icpo->ssp.cout_cpu_req &= ~TX_SUSP;
	set_bit(TTY_IO_ERROR, &tty->flags);
	if (tty->ldisc.flush_buffer)
		(tty->ldisc.flush_buffer)(tty);
#ifdef DEBUG
	printk("after ldisc.flush_buffer for device %d\n",d);
#endif
	chanoff(mpc);
#ifdef DEBUG
	printk("chanoff for device %d\n", d);
#endif
	eqnx_flush_buffer_locked(tty);
	if(win16)
	      mega_pop_win(4);
	tty->closing = 0;
	tty->driver_data = (void *) NULL;
	mpc->mpc_tty = (struct tty_struct *) NULL;
	if (mpc->openwaitcnt){
		if (mpc->close_delay){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			eqnx_delay(mpc->close_delay);
			spin_lock_irqsave(&mpd->mpd_lock, flags);
		}
#ifdef DEBUG
		printk("eqnx_close:waking up on open_wait for device %d\n",d);
#endif
		mpc->open_wait_wait = 0;
		wake_up_interruptible(&mpc->open_wait);
	}
	if (mpd->mpd_board_def->asic != SSP64)  /* only applies to SSP2/SSP4 */
		if (mpc->xmit_buf){
			free_page((unsigned long) mpc->xmit_buf);
			mpc->xmit_buf = 0;
		}
	mpc->flags &= ~(ASYNC_NORMAL_ACTIVE | ASYNC_CLOSING);
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	mpc->flags &= ~ASYNC_CALLOUT_ACTIVE;
#endif
#ifdef DEBUG
	printk("eqnx_close:waking up on close_wait for device %d\n",d);
#endif
	wake_up_interruptible(&mpc->close_wait);
	write_wakeup_deferred = 0;
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	MOD_DEC_USE_COUNT;
#else
	/* 2.6 kernels */
	module_put(THIS_MODULE);
#endif
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
	return;
}

/*
** eqnx_write(tty, from_user, buf, count)
**
** Write routine. Take the data from the user and put it in the sst 
** queue. 
*/
#if	(LINUX_VERSION_CODE < 132618)
static int eqnx_write(struct tty_struct * tty, int from_user,
           const unsigned char *buf, int count)
#else
static int eqnx_write(struct tty_struct * tty, 
           const unsigned char *buf, int count)
#endif
{
	struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	int d, written = 0, c = 0;
	unsigned char  *b;
	int space, nx, win16, datascope = 0;
	volatile icpoaddr_t icpo;
	volatile icpgaddr_t icpg;
	volatile icpqaddr_t icpq;
	unsigned char oldreg;
	int qsize;
	struct mpdev *mpd;
	unsigned long flags;
#ifdef MIDNIGHT
        unsigned char *dst_addr;
        int align, bytes;
#endif
#if	(LINUX_VERSION_CODE < 132618)
	int	ret;
#endif
#ifdef DEBUG
	printk("eqnx_write(tty=%d,count=%d)\n", 
		(unsigned int) SSTMINOR(mpc->mpc_major, mpc->mpc_minor), 
		count);
#endif
	if ((tty == (struct tty_struct *) NULL) || (eqnx_tmpwritebuf == (char *) NULL))
		return(0);
	if (tty == eqnx_txcooktty)
		eqnx_flush_chars(tty);
	if (mpc == (struct mpchan *) NULL)
		return(-ENODEV);
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return(-ENODEV);
	/*
	** lock mpdev board lock
	*/
	mpd = mpc->mpc_mpd;
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	icpo = mpc->mpc_icpo;
	icpg = (icpgaddr_t)icpo;
	icpq = &icpo->ssp.cout_q0;
	win16 = mpc->mpc_mpd->mpd_nwin;
	if(win16)
		mega_push_win(mpc, 0);
	qsize = mpc->mpc_txq.q_size;
	space = qsize - (mpc->mpc_count + Q_data_count) -1 ;
	if (!tty->stopped && !tty->hw_stopped && space >= (qsize >> 4)) {
	
		if ((mpd->mpd_board_def->asic != SSP64) && 
				/* only applies to SSP2/SSP4 */
				(mpc->xmit_cnt))
			goto ssp_end;
#ifdef DEBUG
		printk("eqnx_write: device = %d, q_size = %d, space = %d \n",
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor),
			mpc->mpc_txq.q_size,space);
#endif
		c = MIN(space, count);
		b = (unsigned char *) buf;
#if	(LINUX_VERSION_CODE < 132618)
		if (from_user) {
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			down(&eqnx_tmpwritesem);
			ret = eqn_from_user(eqnx_tmpwritebuf, (void *) buf, c);
			b = eqnx_tmpwritebuf;
			up(&eqnx_tmpwritesem);
			/* In case we got pre-empted */
			c = MIN(c, space);
			if (ret) {
				return(-EFAULT);
			}
			spin_lock_irqsave(&mpd->mpd_lock, flags);
		}
#else
		memcpy(eqnx_tmpwritebuf, buf, c);
		b = eqnx_tmpwritebuf;
		c = MIN(c, space);
#endif

		mpc->mpc_flags |= MPC_BUSY;
		if ((mpc->mpc_flags & MPC_DSCOPEW)
			&& (dscope[1].chan == mpc - meg_chan)) datascope=1;
		/*
 		* Blindingly fast block copy direct from 
 		* driver buffer to on-board buffers.
 		*/

		if(win16)
			mega_push_win(mpc, 2);
		nx = MIN(c, (mpc->mpc_txq.q_end - mpc->mpc_txq.q_ptr +1));
#ifdef MIDNIGHT
                dst_addr = mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr;
                align = ((unsigned long)dst_addr) % 4;
                if (align) {
                  bytes = MIN(nx,(4-align));
                  memcpy(dst_addr, b, bytes);
                }
                else {
                  bytes = 0;
                }
                if (nx > bytes) {
                  memcpy(dst_addr+bytes, b+bytes, nx-bytes);
                }
#else
		memcpy((mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr), 
			b, nx);
#endif
		mpc->mpc_txq.q_ptr += nx;
		if(mpc->mpc_txq.q_ptr > mpc->mpc_txq.q_end)
			mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
		if (datascope) {
			int room,move_cnt;
			int d=1;
			unsigned char *ubase = b;
			room = DSQSIZE - ((dscope[d].next - dscope[d].q.q_ptr) 
				& (DSQMASK));
			if (nx > room)
				dscope[d].status |= DSQWRAP;
                	else
                        	room = nx;
			while (room > 0) {
				move_cnt = MIN(room,
				dscope[d].q.q_end - dscope[d].next + 1);
				memcpy(dscope[d].q.q_addr + dscope[d].next, 
					ubase, move_cnt);
                        	dscope[d].next += move_cnt;
                        	if (dscope[d].next > dscope[d].q.q_end)
                                	dscope[d].next = dscope[d].q.q_begin;
                                ubase += move_cnt;
                                room -= move_cnt;
			
	
			} /* while room */ 
			dscope[d].scope_wait_wait = 0;
			wake_up_interruptible(&dscope[d].scope_wait);
		} /* if MPC_DSCOPE */
		c -= nx;
		count -= nx;
		written += nx;
		buf += nx;
		b += nx;
		if( c ) {
#ifdef MIDNIGHT
                  dst_addr = mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr;
                  align = ((unsigned long)dst_addr) % 4;
                  if (align) {
                        bytes = MIN(c,(4-align));
                        memcpy(dst_addr, b, bytes);
                  }
                  else {
                        bytes = 0;
                  }
                  if (c > bytes) {
                        memcpy(dst_addr+bytes, b+bytes, c-bytes);
                  }
#else
			memcpy(mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr, 
				b, c);
#endif
			mpc->mpc_txq.q_ptr += c;
			if(mpc->mpc_txq.q_ptr > mpc->mpc_txq.q_end)
				mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
			if (datascope) {
				int room,move_cnt;
				int d=1;
				unsigned char *ubase = b;
				room = DSQSIZE - ((dscope[d].next - 
					dscope[d].q.q_ptr) & (DSQMASK));
				if (c > room)
					dscope[d].status |= DSQWRAP;
                		else
                        		room = c;
				while (room > 0) {
					move_cnt = MIN(room,dscope[d].q.q_end - 
						dscope[d].next + 1);
					memcpy(dscope[d].q.q_addr + 
						dscope[d].next, ubase,move_cnt);
                        		dscope[d].next += move_cnt;
                        		if (dscope[d].next > dscope[d].q.q_end)
                                		dscope[d].next = 
							dscope[d].q.q_begin;
                                	ubase += move_cnt;
                                	room -= move_cnt;
				} /* while room */
				dscope[d].scope_wait_wait = 0;
				wake_up_interruptible(&dscope[d].scope_wait);
			} /* if MPC_DSCOPE */
			count -= c;
			written += c;
			b += c;
			buf += c;
		}
		if(win16)
			mega_pop_win(24);
		oldreg = tx_cpu_reg;
		tx_cpu_reg |= TX_SUSP;
		cur_chnl_sync(mpc);
		if(Q_data_count < 9) {
			if ((icpo->ssp.cout_int_save_togl & 0x4) == 
				(icpo->ssp.cout_cpu_req & 0x4)){
				tx_cpu_reg ^= (CPU_SND_REQ);
			mpc->mpc_cout_events &= ~EV_CPU_REQ_DN;
			}
		}
		Q_data_count += (c + nx);
		space -= (c + nx);
#ifdef DEBUG
		printk("eqnx_write: wrote %d chars for device %d\n", (c + nx), 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
#ifdef DEBUG
		printk("q data count %d, space %d for device %d\n", 
			Q_data_count,space, SSTMINOR(mpc->mpc_major, 
				mpc->mpc_minor));
#endif
		if (!(oldreg & TX_SUSP))
			tx_cpu_reg &= ~TX_SUSP;
		mpc->mpc_output += (c + nx);
		tx_cie |= (ENA_TX_EMPTY_Q0| ENA_TX_LOW_Q0);
	}
ssp_end:
	if(win16)
		mega_pop_win(25);
	if (mpd->mpd_board_def->asic ==  SSP64) { /* only applies to SSP64 */
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		return(written);
	}
	if (!count)
		goto end;
	while(1){
#ifdef DEBUG
		printk("trying to fill xmit_buf with %d\n", count);
#endif
		c = MIN(count, MIN(XMIT_BUF_SIZE - mpc->xmit_cnt - 1,
				XMIT_BUF_SIZE - mpc->xmit_head));
		if (c <= 0)
			break;
		b = (unsigned char *) buf;
#if	(LINUX_VERSION_CODE < 132618)
		if (from_user){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			down(&eqnx_tmpwritesem);
			ret = eqn_from_user(eqnx_tmpwritebuf, b, c);
			b = eqnx_tmpwritebuf;
			up(&eqnx_tmpwritesem);
			/* In case we got pre-empted */
			c = MIN(c, MIN(XMIT_BUF_SIZE - mpc->xmit_cnt - 1,
				XMIT_BUF_SIZE - mpc->xmit_head));
			if (ret) {
				return(-EFAULT);
			}
			spin_lock_irqsave(&mpd->mpd_lock, flags);
		}
#else
		memcpy(eqnx_tmpwritebuf, b, c);
		b = eqnx_tmpwritebuf;
		c = MIN(c, MIN(XMIT_BUF_SIZE - mpc->xmit_cnt - 1,
			XMIT_BUF_SIZE - mpc->xmit_head));
#endif

		if (mpc->xmit_buf == NULL)
			break;
		memcpy(mpc->xmit_buf + mpc->xmit_head, b, c);
		mpc->xmit_head = (mpc->xmit_head + c) & (XMIT_BUF_SIZE-1);
		mpc->xmit_cnt += c;
		buf += c;
		count -= c;
		written += c;
#ifdef DEBUG
		printk("eqnx_write: wrote %d chars for device %d in xmit_buf\n",
				c, d);
#endif
	}	
end:
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);

#ifdef DEBUG
	printk("eqnx_write: return value = %d chars and space = %d for device %d\n", 
		written, space, d);
#endif
	return(written);
}

/*
** eqnx_put_char(tty, ch)
**
** Copy individual output characters to a temporary local buffer.
** This buffer will be copied to the board with memcpy to save time.
*/
static void eqnx_put_char(struct tty_struct *tty, unsigned char ch)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	unsigned long flags;
#ifdef DEBUG
	printk("eqnx_put_char(tty=%x,ch=%x)\n", 
		(unsigned int) SSTMINOR(mpc->mpc_major, mpc->mpc_minor), 
		(int) ch);
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;
	if (tty != eqnx_txcooktty) {
		if (eqnx_txcooktty != (struct tty_struct *) NULL)
			eqnx_flush_chars(eqnx_txcooktty);
		eqnx_txcooktty = tty;
	}
	mpd = mpc->mpc_mpd;
	if (mpd == (struct mpdev *) NULL)
		return;
	if (mpd->mpd_board_def->asic != SSP64) {  
		/* only applies to SSP2/SSP4 */
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		if (mpc->xmit_head >= XMIT_BUF_SIZE - 1){
			eqnx_flush_chars_locked(tty);
			eqnx_txcooktty = tty;
		}
		if (mpc->xmit_buf != NULL) {
			mpc->xmit_buf[mpc->xmit_head++] = ch;
			mpc->xmit_head &= XMIT_BUF_SIZE-1;
			mpc->xmit_cnt++;
		}
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);

		if (write_wakeup_deferred) {
			write_wakeup_deferred = 0;
			tty->ldisc.write_wakeup(tty);
		}
	} else{
		if (eqnx_txcooksize >= XMIT_BUF_SIZE -1){
			eqnx_flush_chars(eqnx_txcooktty);
			eqnx_txcooktty = tty;
		}
		eqnx_txcookbuf[eqnx_txcooksize++] = ch;
	}
}

/*
** eqnx_flush_chars(tty)
**
** Move temp buffer to the board, and clear for next port.  
** mpdev board lock ** MUST NOT ** be held			 
*/
static void eqnx_flush_chars(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	unsigned int d;
	unsigned long flags;

	if (tty == (struct tty_struct *) NULL) return;
	if (mpc == (struct mpchan *) NULL) return;

	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor); 
	if(d > nmegaport*NCHAN_BRD) return;

	if (eqnx_txcooktty == NULL)
		return;
	else {
		mpd = mpc->mpc_mpd;

#ifdef	DEBUG_LOCKS
		if (spin_is_locked(&mpd->mpd_lock)) {
			printk("LOCK Failure: mpd board lock already locked in eqnx_flush_chars()\n");
		}
#endif	/* DEBUG_LOCKS */
		
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		eqnx_flush_chars_locked(tty);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);

		if (write_wakeup_deferred) {
			write_wakeup_deferred = 0;
			tty->ldisc.write_wakeup(tty);
		}
	}
}

/*
** eqnx_flush_chars_locked(tty)
**
** Move temp buffer to the board, and clear for next port.  
** mpdev board lock ** MUST ** be held			 
*/
static void eqnx_flush_chars_locked(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev	*mpd;
	unsigned int	d;

#ifdef DEBUG
	printk("eqnx_flush_chars(tty=%x)\n", 
		(unsigned int) SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;


#ifdef	NOTNEEDED
	/* removed - is there a good reason for this test ? */
	/* it breaks echo - mikes */
	if (!O_OPOST(tty)){
#ifdef DEBUG
	printk("opost:eqnx_flush_chars(tty=%x)\n", 
		(unsigned int) SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
		return;
	}
#endif	/* NOTNEEDED */

	/* Shashi: 03/05/98, lets check tty before we get to mpc */
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if (d > nmegaport*NCHAN_BRD)
		return;
	/* Shashi 03/05/98, lets check mpc for eqnx_txcooktty */
	if (eqnx_txcooktty == NULL)
		return;
	else{
		mpd = mpc->mpc_mpd;

#ifdef	DEBUG_LOCKS
		if (!(spin_is_locked(&mpd->mpd_lock))) {
			printk("LOCK Failure: mpd board lock NOT locked in eqnx_flush_chars_locked()\n");
		}
#endif	/* DEBUG_LOCKS */
		
		if (mpd->mpd_board_def->asic == SSP64) { 
			/* only applies to SSP64 */
			unsigned int cooksize = eqnx_txcooksize;
			eqnx_txcooksize = 0;
			eqnx_txcookrealsize = 0;
			eqnx_txcooktty = (struct tty_struct *) NULL;
			if (cooksize == 0)
				return;
#ifdef DEBUG
			printk("eqnx_flush_chars: calling sst_write\n");
#endif
			sst_write(mpc, eqnx_txcookbuf, cooksize);
		}
		else{
			if (mpc->xmit_cnt == 0)
				return;
#ifdef DEBUG
			printk("eqnx_flush_chars: calling sst_write1\n");
#endif
			sst_write1(mpc,EQNX_COOKED);
		}
	}
}

/*
** eqnx_write_room(tty)
**
** return amount of room available for a write
*/
static int eqnx_write_room(struct tty_struct *tty)
{
	register struct mpchan *mpc;
	struct mpdev *mpd;
	icpoaddr_t icpo;
	icpqaddr_t icpq;
	unsigned int space;
	int win16, d;
	unsigned long flags;

	if (tty == (struct tty_struct *) NULL)
		return(0);
	mpc = (struct mpchan *)tty->driver_data;
	if (mpc == (struct mpchan *) NULL)
		return(-ENODEV);
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return(-ENODEV);
#ifdef DEBUG
	printk("eqnx_write_room: device = %d\n", d);
#endif
	mpd = mpc->mpc_mpd;
	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpd->mpd_lock, flags);

	if (mpd->mpd_board_def->asic != SSP64) { 
		/* only applies to SSP2/SSP4 */
		int ret;
		ret = (XMIT_BUF_SIZE - mpc->xmit_cnt -1);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		if (ret < 0)
			return(0);
		return(ret);
	}
	if (tty == eqnx_txcooktty){
		if (eqnx_txcookrealsize != 0){
			space = (eqnx_txcookrealsize - eqnx_txcooksize);
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			return(space);
		}
	}
	win16 = mpd->mpd_nwin;
	if(win16)
		mega_push_win(mpc, 0);
	icpo = mpc->mpc_icpo;
	icpq = &icpo->ssp.cout_q0;
	space = mpc->mpc_txq.q_size - (mpc->mpc_count + Q_data_count) -1 ;
	if (tty == eqnx_txcooktty){
		eqnx_txcookrealsize = space;
		space -= eqnx_txcooksize;
	}
	if(win16)
		mega_pop_win(25);

	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
	return(space);
}

/*
** eqnx_chars_in_buffer(tty)
**
** Return the number of characters buffered to be output
*/
static int eqnx_chars_in_buffer(struct tty_struct *tty)
{
	struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	unsigned long flags;
	icpoaddr_t icpo;
	icpqaddr_t icpq;
	unsigned int count;
	int win16, d;

	if (tty == (struct tty_struct *) NULL)
		return(-ENODEV);
	if (mpc == (struct mpchan *) NULL)
		return(-ENODEV);
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return(-ENODEV);
#ifdef DEBUG
	printk("eqnx_chars_in_buffer: count for device %d = %d\n", 
			d,mpc->xmit_cnt);
#endif
	/*
	** lock mpdev board lock
	*/
	mpd = mpc->mpc_mpd;
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	win16 = mpc->mpc_mpd->mpd_nwin;
	if(win16)
		mega_push_win(mpc, 0);
	icpo = mpc->mpc_icpo;
	icpq = &icpo->ssp.cout_q0;
	count = Q_data_count;
	if (mpc->mpc_mpd->mpd_board_def->asic != SSP64) /* SSP2 / SSP4 only */
		count += mpc->xmit_cnt;
	if(win16)
		mega_pop_win(25);
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#ifdef DEBUG
	printk("eqnx_chars_in_buffer: count for device %d = %d\n", d,count);
#endif
	return(count);
}

/*
** eqnx_flush_buffer(tty)
**
** Flush buffer routine
** mpdev board lock ** MUST NOT ** be held			 
*/
static void eqnx_flush_buffer(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	unsigned long flags;

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;

	mpd = mpc->mpc_mpd;

#ifdef	DEBUG_LOCKS
	if (spin_is_locked(&mpd->mpd_lock)) {
		printk("LOCK Failure: mpd board lock already locked in eqnx_flush_buffer()\n");
	}
#endif	/* DEBUG_LOCKS */
		
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	eqnx_flush_buffer_locked(tty);
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);

	if (write_wakeup_deferred) {
		write_wakeup_deferred = 0;
		tty->ldisc.write_wakeup(tty);
	}
}

/*
** eqnx_flush_buffer_locked(tty)
**
** Flush buffer routine
** mpdev board lock ** MUST ** be held			 
*/
static void eqnx_flush_buffer_locked(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	register icpiaddr_t icpi;
	register icpoaddr_t icpo;
	register icpqaddr_t icpq;
	icpbaddr_t icpb;
	int win16;
	uchar_t cin;
	int d;

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in eqnx_flush_buffer_locked()\n");
	}
#endif	/* DEBUG_LOCKS */

	icpi = mpc->mpc_icpi;
	icpo = mpc->mpc_icpo;
	icpq = &icpo->ssp.cout_q0;
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return;
#ifdef DEBUG
	printk("eqnx_flush_buffer: device = %d\n", d);
#endif
	win16 = mpc->mpc_mpd->mpd_nwin;

	if (tty == eqnx_txcooktty){
		eqnx_txcooktty = (struct tty_struct *) NULL;
		eqnx_txcooksize = 0;
		eqnx_txcookrealsize = 0;
	}
	if(win16) 
	      mega_push_win(mpc, 0);
	/* flush transmit buffers */
	tx_lck_ctrl |= LCK_Q_ACT;
        cur_chnl_sync(mpc);
        tx_dma_stat = 0;
        icpo->ssp.cout_int_fifo_ptr = 0;  
        Q_data_count = 0;
        mpc->mpc_count = 0;
        Q_data_ptr = mpc->mpc_txq.q_begin + mpc->mpc_txbase;
        mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
	mpc->xmit_cnt = mpc->xmit_head = mpc->xmit_tail = 0;
        icpo->ssp.cout_attn_ena = 0x8000;
        tx_lck_ctrl &= ~LCK_Q_ACT;
        mpc->mpc_flags &= ~MPC_BUSY;
	
	/* flush receiver queue */
	/* set rx ptr = tail */
	icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
  	if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  		frame_wait(mpc,2); /* make sure regs are valid */ 
  	cin = icpi->ssp.cin_locks;
	icpi->ssp.cin_locks = cin | 0x03;
	cur_chnl_sync( mpc );
  	icpi->ssp.cin_bank_a.ssp.bank_fifo_lvl &= ~0x8f;
  	icpi->ssp.cin_bank_b.ssp.bank_fifo_lvl &= ~0x8f;
	icpi->ssp.cin_tail_ptr_a = 
	icpi->ssp.cin_tail_ptr_b =  
	mpc->mpc_rxq.q_ptr = rx_next;
        icpi->ssp.cin_locks = cin;
	if(win16)
	 mpc->mpc_rxq.q_ptr &= 0x3fff;
	mpc->mpc_flags |= MPC_RXFLUSH;
	mpc->mpc_block = 0;
	if(win16) 
	        mega_pop_win(5);
	wake_up_interruptible(&tty->write_wait);
	/* signal write wakeup when safe to call ldisc */
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		tty->ldisc.write_wakeup)
		write_wakeup_deferred++;
}

/*
** eqnx_throttle(tty)
**
** Stop queuing input to the line discipline.
*/
static void eqnx_throttle(struct tty_struct * tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	
#ifdef DEBUG
	printk("eqnx_throttle: device = %d\n", SSTMINOR(mpc->mpc_major, 
				mpc->mpc_minor));
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;

#ifdef DEBUG
	printk("eqnx_throttle: setting mpc_block for device %d\n", 
		SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
	mpc->mpc_block = 1;
}

/*
** eqnx_unthrottle(tty)
**
** Resume queuing input to the line discipline.
*/
static void eqnx_unthrottle(struct tty_struct * tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	
#ifdef DEBUG
	printk("eqnx_unthrottle: device = %d\n", SSTMINOR(mpc->mpc_major, 
				mpc->mpc_minor));
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;

	mpc->mpc_block = 0;
}

/*
** eqnx_set_termios(tty, old)
**
** Device termios structure has been modified.
*/
static void eqnx_set_termios(struct tty_struct *tty, 
		struct termios * old)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	int d;
	struct termios *tiosp;
	int win16;
	unsigned long flags;

	if (tty == (struct tty_struct *) NULL)
		return;
	if (mpc == (struct mpchan *) NULL)
		return;
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return;
#ifdef DEBUG
	printk("eqnx_set_termios: trying set termios for %d\n", d);
#endif
	tiosp = tty->termios;
	if ((tiosp->c_cflag == old->c_cflag) && 
		(tiosp->c_iflag == old->c_iflag) &&
		(tiosp->c_cc[VSTOP] == old->c_cc[VSTOP]) &&
		(tiosp->c_cc[VSTART] == old->c_cc[VSTART]))
		return;
#ifdef DEBUG
	printk("eqnx_set_termios: new termios settings for %d\n", d);
#endif
	win16 = mpc->mpc_mpd->mpd_nwin;

	/*
	** lock mpdev board lock
	*/
	mpd = mpc->mpc_mpd;
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	if(win16)
		mega_push_win(mpc, 0);
/* RAMP START --------------------------------------------- */
	/* If RAMP config not completed, don't let any ioctl's in */
	if (ISRAMP(mpc)) {
	/* check break bit */
	/* If not receiving a break, the slot is occupied by a UART */
      
		if ( !(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE) ||
                     (mpc->mpc_mpa_stat & (MPA_INITG_INIT_MODEM | MPA_INITG_MODIFY_SETTINGS))) {
			register icpiaddr_t icpi;
			register icpbaddr_t icpb;
      			icpi = mpc->mpc_icpi;
      			icpb=(rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b 
				: &icpi->ssp.cin_bank_a;
			/* make sure regs are valid */ 
  			if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  				frame_wait(mpc,2); 
         		if (!(icpb->ssp.bank_sigs & 0x01 )) {
				if(win16) mega_pop_win(74);
            			/* Modem present in slot, sleep */
termios_modem_wait:
				mpc->mpc_mpa_call_back_wait++;
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
				/* 2.2 and 2.4 kernels */
				interruptible_sleep_on(&mpc->mpc_mpa_call_back);
#else
				/* 2.6 kernels */
				wait_event_interruptible(mpc->mpc_mpa_call_back,
					mpc->mpc_mpa_call_back_wait == 0);
#endif
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				if eqn_fatal_signal {
					spin_unlock_irqrestore(&mpd->mpd_lock, flags);
					return;
				}
				if(mpc->mpc_mpa_stat & MPA_CALL_BACK_ERROR){
					spin_unlock_irqrestore(&mpd->mpd_lock, flags);
                        		return;
				}
				else if ( !(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE) ||
                     (mpc->mpc_mpa_stat & (MPA_INITG_INIT_MODEM | MPA_INITG_MODIFY_SETTINGS))) {
					goto termios_modem_wait;
				}
				if(win16)
					mega_push_win(mpc, 0);
         		} else {
				if(win16) mega_pop_win(75);
            			/* No modem, return error */
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
				return;
         		}
		}
	}
/* RAMP END ----------------------------------------------- */
#ifdef DEBUG
	printk("set_termios: Calling megaparam for device %d\n", d);
#endif
	megaparam(d);
	if(win16)
	   mega_pop_win(2);
	if ((old->c_cflag & CRTSCTS) && ((tiosp->c_cflag & CRTSCTS) == 0))
		tty->hw_stopped = 0;
	if (((old->c_cflag & CLOCAL) == 0) && (tiosp->c_cflag & CLOCAL)){
		mpc->carr_state = 1;
#ifdef DEBUG
	printk("eqnx_set_termios:waking up on open_wait for device %d\n",d);
#endif
		mpc->open_wait_wait = 0;
		wake_up_interruptible(&mpc->open_wait);
	}
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
}

/*
** eqnx_stop(tty)
**
** Set tty->stopped, suspend tx cpu register
*/
static void eqnx_stop(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	register icpoaddr_t icpo;
	unsigned long flags;
	int win16;
	
#ifdef DEBUG
	printk("eqnx_stop(tty=%d)\n", SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	mpc = tty->driver_data;
	if (mpc == (struct mpchan *) NULL)
		return;
	win16 = mpc->mpc_mpd->mpd_nwin;
	icpo = mpc->mpc_icpo;

	/*
	** lock mpdev board lock
	*/
	mpd = mpc->mpc_mpd;
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	if(win16)
		mega_push_win(mpc,0);
	/* suspend output */
	tx_cpu_reg |= 0x80;
	if(win16)
		mega_pop_win(6);
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
}

/*
** eqnx_start(tty)
**
** Reset tty->stopped, release tx cpu register
*/
static void eqnx_start(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	register icpoaddr_t icpo;
	unsigned long flags;
	int win16;
	
#ifdef DEBUG
	printk("eqnx_start(tty=%d)\n", SSTMINOR(mpc->mpc_major, 
				mpc->mpc_minor));
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	mpc = tty->driver_data;
	if (mpc == (struct mpchan *) NULL)
		return;
	win16 = mpc->mpc_mpd->mpd_nwin;
	icpo = mpc->mpc_icpo;

	/*
	** lock mpdev board lock
	*/
	mpd = mpc->mpc_mpd;
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	if(win16)
		mega_push_win(mpc,0);
	/* resume output */
	tx_cpu_reg &= ~0x80;
	if(win16)
		mega_pop_win(7);
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
}

/*
** eqnx_hangup(tty)
**
** called by tty_hangup() when a hangup is signaled.
** drop control signals, flush buffer, and close port.
*/
static void eqnx_hangup(struct tty_struct *tty)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	unsigned long flags;
	int d;
	int win16;

#ifdef DEBUG
	printk("eqnx_hangup: device = %d\n", 
			(unsigned int) SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif

	if (tty == (struct tty_struct *) NULL)
		return;
	mpc = tty->driver_data;
	if (mpc == (struct mpchan *) NULL)
		return;
	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if(d > nmegaport*NCHAN_BRD)
		return;

	/*
	** lock mpdev board lock
	*/
	mpd = mpc->mpc_mpd;
	spin_lock_irqsave(&mpd->mpd_lock, flags);
	mpc->flags &= ~ASYNC_INITIALIZED;
	win16 = mpc->mpc_mpd->mpd_nwin;
	if(win16) 
	      mega_push_win(mpc, 0);
	if (mpc->flags & ASYNC_NORMAL_ACTIVE)
		*mpc->normaltermios = *tty->termios;
#if (LINUX_VERSION_CODE < 132096)
	if (mpc->flags & ASYNC_CALLOUT_ACTIVE)
		*mpc->callouttermios = *tty->termios;
#endif
	if (tty == eqnx_txcooktty)
		eqnx_flush_chars_locked(tty);
	if (tty->termios->c_cflag & HUPCL ){
		megamodem(d, TURNOFF);
	}
	 mpc->mpc_flags &= ~(MPC_SOFTCAR|MPC_DIALOUT|MPC_MODEM|MPC_DIALIN|MPC_CTS);
	 mpc->mpc_scrn_no = 0;
	 mpc->mpc_icpi->ssp.cin_attn_ena &= ~ENA_DCD_CNG;
	 chanoff(mpc);
	 if(win16)
	      mega_pop_win(8);
	eqnx_flush_buffer_locked(tty);
	set_bit(TTY_IO_ERROR, &tty->flags);
	mpc->flags &= ~ASYNC_NORMAL_ACTIVE;
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	mpc->flags &= ~ASYNC_CALLOUT_ACTIVE;
#endif
	mpc->mpc_tty = (struct tty_struct *) NULL;
	mpc->refcount = 0;
#ifdef DEBUG
	printk("eqnx_hangup: eqnx_close:waking up on open_wait for device %d\n",d);
#endif
	if ((mpd->mpd_board_def->asic != SSP64) && 
		/* only applies to SSP2/SSP4 */
		(mpc->xmit_buf)){
		free_page((unsigned long) mpc->xmit_buf);
		mpc->xmit_buf = 0;
		mpc->xmit_cnt = 0;
		mpc->xmit_head = 0;
		mpc->xmit_tail = 0;
	}
	mpc->open_wait_wait = 0;
	wake_up_interruptible(&mpc->open_wait); /*Shashi 02/09/98 */
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);

	if (write_wakeup_deferred) {
		write_wakeup_deferred = 0;
		tty->ldisc.write_wakeup(tty);
	}
}

#if	(LINUX_VERSION_CODE >= 132608)
/* 2.6+ kernels */
/*
** eqnx_tiocmget(tty, file)
**
** return state of control signals.
*/
static int
eqnx_tiocmget(struct tty_struct *tty, struct file *file)
{
	unsigned char status;
	unsigned int result = 0;
	unsigned long flags;
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;

	if (mpc == (struct mpchan *) NULL)
		return result;

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	status = eqn_cs_get(mpc);
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);

	result = (((status & 0x20) ? TIOCM_RTS : 0)
		| ((status & 0x10) ? TIOCM_DTR : 0)
		| ((status & 0x1) ? TIOCM_CAR : 0)
		| ((status & 0x8) ? TIOCM_RI : 0)
		| ((status & 0x4) ? TIOCM_DSR : 0)
		| ((status & 0x2) ? TIOCM_CTS : 0));

	return result;
}

/*
** eqnx_tiocmset(tty, file, set, clear)
**
** set state of control signals.
*/
static int
eqnx_tiocmset(struct tty_struct *tty, struct file *file, unsigned int set,
	unsigned int clear)
{
	unsigned int temp;
	unsigned long flags;
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	icpoaddr_t icpo = mpc->mpc_icpo;

	if (mpc == (struct mpchan *) NULL)
		return 0;

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	GET_CTRL_SIGS(mpc, temp);
	if (set & TIOCM_RTS)
		temp |= TX_RTS | TX_HFC_RTS;
	if (set & TIOCM_DTR)
		temp |= TX_DTR | TX_HFC_DTR;
	if (clear & TIOCM_RTS)
		temp &= ~(TX_RTS | TX_HFC_RTS);
	if (clear & TIOCM_DTR)
		temp &= ~(TX_DTR | TX_HFC_DTR);

	if ( ((icpo->ssp.cout_int_opost & 0x8) && (temp & TX_SND_CTRL_TG)) ||
	   (!(icpo->ssp.cout_int_opost & 0x8) && !(temp & TX_SND_CTRL_TG)) )  
			temp ^= TX_SND_CTRL_TG;
	SET_CTRL_SIGS(mpc, temp);
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
	return(0);
}
#endif

/*
** eqnx_init(kmem_start)
**
** initialization routine
*/
long eqnx_init(long kmem_start)
{
	register struct mpdev *mpd;
	register struct mpchan *mpc;
	volatile register icpiaddr_t icpi;
	volatile icpoaddr_t icpo;
	volatile icpgaddr_t icpg;
	volatile icpqaddr_t icpq;
	icpaddr_t icp;
	volatile struct hwq_struct *hwq;
	mpaddr_t mp;
	int  i, j, k, lmx;
	uchar_t cchnl;
	volatile unsigned char *chnl_ptr;
	ushort_t no_cache;
	ushort_t no_icp;
	int ii, jj;
	int addr;
	struct pci_csh *cshp;
	unsigned short numboards = 0;
	unsigned char  config;
	unsigned char  rev_id;
	unsigned short dev_id;
	paddr_t	base_addr;
	unsigned char  slot;
	unsigned char c_code;  /* PatH - Multimodem country code */
	int duplicate, di;
	unsigned long flags;

#if	defined(ISA_ENAB) || defined(MCA_ENAB)
	register int b;
	uchar_t bid, cbid, lbid, nports;
	struct isa_cnfg_parms isa_parms;
	int b0 = 0;
	int eisa_or_isa = 0;
	int mca_bus = 0;
	ushort_t eisa_cfg[EISA_REGS_LEN];
	uchar_t mca_cfg[ MCA_REGS_LEN ];
#endif	/* ISA_ENAB || MCA_ENAB */

        int ssp_channels = 0;
        volatile uchar_t *bus_ctrl_p;
	int lmx_factor;
	printk("Loading %s Version %s\n", Version, VERSNUM);
	printk("%s\n", Copyright);

	ramp_admin_poll = RAMP_FAST_TIME;

	strncpy(ist.drvr_ver, VERSNUM, 16);
	ist.drvr_type = 1 ; /* Character Driver */
	if (kmem_start)
		ist.drvr_bind_type = 1 ; /* Driver Linked with kernel */
	else
		ist.drvr_bind_type = 2 ; /* Loadable driver */
	ist.drvr_obj_type = 0x0C; /* LINUX */
	ist.ctlr_max_num = maxbrd;
	ist.ctlr_found = 0;

	spin_lock_init(&ramp_lock);
	spin_lock_init(&eqnx_mem_hole_lock);

	/* Make sure all boards start with "dead" status - Monty 6/12/96 */
	for (ii=0; ii < maxbrd; ii++) {
		meg_dev[ii].mpd_alive = 0;
		spin_lock_init(&meg_dev[ii].mpd_lock);
	}

#if	(LINUX_VERSION_CODE >= 132096)
	/* 2.4 kernels and after */
	init_MUTEX(&eqnx_tmpwritesem);
#endif

	nmegaport = 0;
	nicps = 0;
	nextmin = 0;
	/* Start with Space.c info on bus_type */  

#if	defined(ISA_ENAB) || defined(MCA_ENAB)
	if (io_bus_type == EISA_BUS || io_bus_type == ISA_BUS) 
		eisa_or_isa = 1;
#endif	/* ISA_ENAB || MCA_ENAB */

	if ((eqnPCIcsh = (char *)vmalloc( pcisize * maxbrd)) 
		== (char *) NULL){
		printk("EQUINOX: Failed to allocate virtual address space of size %d for eqnPCIcsh\n", pcisize * maxbrd);
		return(-1);
		}
	memset(eqnPCIcsh, 0, pcisize * maxbrd);
	cshp = (struct pci_csh *)&eqnPCIcsh[0];
	numboards = eqx_pci_buspresent(cshp);

#if	defined(ISA_ENAB) || defined(MCA_ENAB)
	if (io_bus_type == MCA_BUS) 
		mca_bus = 1;

	oper_input.sys_bus_type = io_bus_type; /* From space.c */ 
	oper_input.num_cntrls = maxbrd; /* From space.c */ 
	oper_input.quiet_mode = 0xff;  /* No messages for now  */
	oper_input.isa_def_specd = 1; /* Dynamic always for now */ 
#endif	/* ISA_ENAB || MCA_ENAB */

	if (numboards) {
		for(i = 0; i < numboards; i++, cshp++) {
			/* 
		 	 * See if board configured by checking the
		 	 * "Memory Space Control" bit in the Command
		 	 * register (bit 1 @ offset 0x04)
		 	 */
	  		mpd = &meg_dev[nmegaport];

			dev_id = cshp->cs.csh00.dev_id;
			mpd->mpd_board_def = find_board_def(dev_id, PCI_BUS);

			if (mpd->mpd_board_def == NULL)
				mpd->mpd_board_def = &unknown_board;

			/* Save model number only */
	  		mpd->mpd_id = dev_id;
			printk("EQNX SST Board %d : id %4.4x (%s)\n",i, 
					dev_id, mpd->mpd_board_def->name);

			ist.ctlr_array[nmegaport].ctlr_model = 
				mpd->mpd_id;
			ist.ctlr_array[nmegaport].ctlr_model_ext = 
				((mpd->mpd_id & 0xFF00) >> 8);

			dev_id &= 0xff; /* low byte only */
			config = cshp->cs.csh00.command;
			rev_id = cshp->cs.csh00.rev_id;
			base_addr=cshp->cs.csh00.base_addr_reg0 &
				PCI_BASE_ADDR_MASK;
			/*
			 * This code checks for duplicate boards.
			 * This happens because some brain-dead
			 * PCI controllers "see" multiple busses
			 * when there aren't multiple busses.
			 * So, they see the same bus multiple times,
			 * reporting our board on all the busses.
			 */
			duplicate = 0;
			for(di = 0; di < i; di++) {
				struct mpdev *mpd = &meg_dev[di];

				if (base_addr == mpd->mpd_pmem) {

					duplicate = 1;
					break;
				}
			}
			if (duplicate)
				continue;
			slot = cshp->slot;
			/* Make sure base address was really 
			   configured */
			if (!base_addr){
    				printk("SST:PCI Base Addr not configured\n");
				continue;
			}

			/* 
		 	 * Look at the Memory Space Control bit to
		 	 * see if this has been configured. 
		 	 */
			if (!(config & PCI_MSC)) {
				continue;
			}

			/* Make sure we don't find more boards than 
			 * our max. */
			if (++ist.ctlr_found > maxbrd) {
				ist.ctlr_array[nmegaport].cnfg_state=2; 
				/* maxbrd exceeded */
				ist.ctlr_array[nmegaport].cnfg_fail = 5; 
				continue;
			}
			else
				ist.ctlr_array[nmegaport].cnfg_state=1; 

			/* Mark board as being alive */
	  		mpd->mpd_alive = true;

			/* revision */
			ist.ctlr_array[nmegaport].ctlr_rev = rev_id;

			/*  PCI */
			mpd->mpd_bus =  PCI_BUS;
			ist.ctlr_array[nmegaport].ctlr_bus_type =  
				PCI_BUS; 
	  		mpd->mpd_slot = slot;
			ist.ctlr_array[nmegaport].ctlr_loc = 
				mpd->mpd_slot;
			ist.ctlr_array[nmegaport].ctlr_mca_port = 0;

			/*  PCI CONFIG */
			ist.ctlr_array[nmegaport].cnfg_init = 1; 

			mpd->mpd_nicps = mpd->mpd_board_def->number_of_asics;
			nicps += mpd->mpd_nicps;

			/* Save the physical address */
			mpd->mpd_pmem = base_addr;
			ist.ctlr_array[nmegaport].cnfg_physc_base = 
				mpd->mpd_pmem;

			/* Get the memory size */
                       	mpd->mpd_addrsz = pci_mem_size(mpd->mpd_board_def);
                       	ist.ctlr_array[nmegaport].cnfg_addr_size = 
				mpd->mpd_addrsz;

			/* PCI is always Flat */;
                       	mpd->mpd_nwin = HA_FLAT;
			ist.ctlr_array[nmegaport].cnfg_mode = 2;

			if (mpd->mpd_board_def->number_of_asics == 4)
  				/* SSP4 SST-16P with no panel */
     				/* SSP4 SST-16P with 16RJ */
     				/* SSP4 SST-16P with 16DB */
     				/* SSP4 SST-16P with 16DB9 */
				mpd->mpd_memsz = mpd->mpd_addrsz;
			else /* DRAM memory size is one-half memory size */
				mpd->mpd_memsz = mpd->mpd_addrsz/2;

			/* Not used in HA_FLAT mode */
                       	mpd->mpd_pgctrl = mpd->mpd_slot + 2;

			/* PCI memory width is always 32 */
			mpd->mpd_mem_width = 32;

			/* 16 bit mode */
			ist.ctlr_array[nmegaport].cnfg_isa_bus_sz=0x10; 

			mpd->mpd_nchan = mpd->mpd_board_def->number_of_ports;
			mpd->mpd_minor = nextmin;
			nextmin += NCHAN_BRD;

			nmegaport++;
		} /* End i < numboards */
	} /* End if numboards */

#if	defined(ISA_ENAB) || defined(MCA_ENAB)
	if (eisa_or_isa) {
		j = 8;
		/* loop thru EISA slots */
		for (b = 0; b < j ; b++) {
		  switch(eisa_brd_found(b, eisa_cfg))
		  {
		  case true:
			if (++ist.ctlr_found > maxbrd) {
				ist.ctlr_array[nmegaport].cnfg_state = 2; 
				ist.ctlr_array[nmegaport].cnfg_fail = 5; /* maxbrd exceeded */
	   		continue;
			}
			else
				ist.ctlr_array[nmegaport].cnfg_state = 1; 
		  	mpd = &meg_dev[nmegaport];
		  	mpd->mpd_alive = true;
		  	mpd->mpd_id = eisa_cfg[7] &0xFFF8 ; /* Save model number only */
			ist.ctlr_array[nmegaport].ctlr_model = mpd->mpd_id;
			ist.ctlr_array[nmegaport].ctlr_model_ext = 
				((mpd->mpd_id & 0xFF00) >> 8);
			ist.ctlr_array[nmegaport].ctlr_rev = eisa_cfg[7] & 0x7; /* revision */
			mpd->mpd_bus = EISA_BUS;
			ist.ctlr_array[nmegaport].ctlr_bus_type = EISA_BUS; /* EISA */
			mpd->mpd_board_def = find_board_def(mpd->mpd_id,
				EISA_BUS);

			if (mpd->mpd_board_def == NULL)
				mpd->mpd_board_def = &unknown_board;

			printk("EQNX SST Board %d : id %4.4x (%s)\n",nmegaport, 
					mpd->mpd_id, mpd->mpd_board_def->name);

		  	mpd->mpd_slot = b;
			ist.ctlr_array[nmegaport].ctlr_loc = mpd->mpd_slot;
			ist.ctlr_array[nmegaport].ctlr_mca_port = 0;
			ist.ctlr_array[nmegaport].cnfg_init = 1; /* EISA CONFIG */
			mpd->mpd_nicps = mpd->mpd_board_def->number_of_asics;
			nicps += mpd->mpd_nicps;
			mpd->mpd_pmem = eisa_base_paddr(eisa_cfg);
			ist.ctlr_array[nmegaport].cnfg_physc_base = 
				mpd->mpd_pmem;
                        if ((mpd->mpd_addrsz = eisa_mem_size(eisa_cfg, 0))== 0)
                        {
                            ist.ctlr_array[nmegaport].cnfg_state = 2;
                            ist.ctlr_array[nmegaport].cnfg_fail = 6; /* fail bad address */
                            nextmin += NCHAN_BRD;
                            nmegaport++;
                            mpd->mpd_alive = false;
			    printk("SSM: failed bad address\n");
                            continue;
                        }

                        ist.ctlr_array[nmegaport].cnfg_addr_size = mpd->mpd_addrsz;
                        if ((mpd->mpd_nwin = eisa_mem_strat(eisa_cfg)) == HA_WIN16)
				ist.ctlr_array[nmegaport].cnfg_mode = 1 /* Paged */;
			else
				ist.ctlr_array[nmegaport].cnfg_mode = 2 /* Flat */;
			mpd->mpd_memsz = eisa_mem_size(eisa_cfg, 1);

			if (mpd->mpd_board_def->asic == SSP64)
                        	mpd->mpd_pgctrl = mpd->mpd_slot * 0x1000 + 0xC80 + 2;
			else
                        	mpd->mpd_pgctrl = mpd->mpd_slot * 0x1000 + 0xC80 + 8;
				mpd->mpd_mem_width = 16;
				ist.ctlr_array[nmegaport].cnfg_isa_bus_sz = 0x10; /* 16 bit mode */
				mpd->mpd_nchan = 
					mpd->mpd_board_def->number_of_ports;
				mpd->mpd_minor = nextmin;
				nextmin += NCHAN_BRD;
				nmegaport++;
		  	break;
		  case -1:
			printk("Run EISA configuration utility for Equinox board in slot %d.\n", b);
			break;
		  default:
			break;
		  }
		}	
	}
#endif	/* ISA_ENAB || MCA_ENAB */

#ifdef	ISA_ENAB
#if	(LINUX_VERSION_CODE < 132608)
#if	(LINUX_VERSION_CODE >= 132096)
#ifdef	CONFIG_ISAPNP
	{
	/*
	** ISA plug-and-play in linux kernels 2.4+
	*/
	extern struct list_head isapnp_devices;
	struct list_head *list;
	struct pci_dev *dev;

	list = isapnp_devices.next;
	while (list != &isapnp_devices) {
		dev = pci_dev_g(list);	
		if ((dev->vendor == ISAPNP_VENDOR('E','Q','X')) &&
			(pnp_found < MAX_PNP_DEVS)) {
			pnp_found_devs[pnp_found++] = dev;
#if	(LINUX_VERSION_CODE < 132608)
			/* 2.4 kernels only */
			dev->prepare(dev);
			dev->activate(dev);
#endif
		}
		list = list->next;
	}
	}
#endif	/* CONFIG_ISAPNP */
#endif
#endif

/*
** Memory returned from PnP configured boards is unreliable
** do not use MegAddr		
**			Mikes 12/19/01
*/

#ifdef	RELIABLE_PNP
/* Search for ISA Controllers */
/* First scan for any Plug and Play (PnP) configured boards 
   and use that ISA hole (if MegAddr not already configured) */
	if (eisa_or_isa && !MegAddr) {
		for(k=0; k < 16; k++) {
			bid = inb(portn[k]);
			cbid = ~inb(portn[k] + 1);
			lbid = ~inb(portn[k] + 1);
			if((bid == cbid || bid == lbid) && (cbid != lbid)) {
				unsigned long int SavAddr = 0;
				if ((bid & 0xf8) == 0xF8) { /* SSP4 */
				if (inb(portn[k] + 4) & 0x80) { 
					unsigned char port7,port9,porta;
					port7 = inb(portn[k] + 0x7);
					port9 = inb(portn[k] + 0x9);
					porta = inb(portn[k] + 0xA);
					SavAddr = (porta << 16) | ((port9 & 0xc0) << 8) ;
					if (SavAddr && ((port7 & 0xC0) == 0xC0))
						MegAddr = SavAddr;
					printk("MegAddr = %x\n", (unsigned int) MegAddr);
					break;
				}
			} else { /* check for 64 or 128 port board, rev 1 or greater */
				if ((((bid & 0xf8) == 0x10) || ((bid & 0xf8) == 0x08)) && 
				    ((bid & 0x7) > 0) && ((inb(portn[k] + 2) & 0xC0) == 0xC0)) { 
					unsigned char port4,port5,port7;
					port4 = inb(portn[k] + 0x4);
					port5 = inb(portn[k] + 0x5);
					port7 = inb(portn[k] + 0x7);
					SavAddr = (port5 << 16) | ((port4 & 0xc0) << 8) ;
					if (SavAddr && ((port7 & 0xC0) == 0xC0))
						MegAddr = SavAddr;
					printk("MegAddr = %x\n", (unsigned int) MegAddr);
					break;
				}
			} /* end if else */ 
			} /* end if (bid) */
		} /*for*/
	} /*if eisa or isa*/
#endif	/* RELIABLE_PNP */
#endif	/* ISA_ENAB */

#if	defined(ISA_ENAB) || defined(MCA_ENAB)
	if (eisa_or_isa) {
		int pnp = 0; /* default */
		for(k=0; k < 16; k++) {
#if	(LINUX_VERSION_CODE < 132608)
			/* 2.2/2.4 kernels */
			if(check_region(portn[k], 8))
				continue;
#endif
			bid = inb(portn[k]);
			cbid = ~inb(portn[k] + 1);
			lbid = ~inb(portn[k] + 1);
                        if((bid == cbid || bid == lbid) && (cbid != lbid)) {
				if (++ist.ctlr_found > maxbrd) {
					ist.ctlr_array[nmegaport].cnfg_state=2; 
					/* maxbrd exceeded */
					ist.ctlr_array[nmegaport].cnfg_fail = 5;
		   			continue;
				}

				mpd = &meg_dev[nmegaport];
				if ((bid & 0xF8) >= SSP2_ASIC) {
					uchar_t u1;
					ushort_t v1, v2;
					uint_t pp;
					/* Get qualifier bits */
     					pp = portn[k];
     					u1 = inb( (pp + 8));  /* port 8/9 */
     					outb( (u1 | 0x80),(pp + 8)); 
     					v1 = inb( (pp));  /* port 0 */
     					v1 = (v1 & 0x07);/*  set lower 3 bits */
					/* toggle rmod */
     					outb( ( u1 & ~0x80 ),(pp + 8));
     					v2 = inb( (pp));  /* port 0 */
     					v2 = ((v2 & 0x07) << 3); /* set upper bits */
					v1 |= v2;
					mpd->mpd_id = ((bid | (v1 << 8)) 
								& 0xfff8);
					/* revision does not live in same place 
						on SSP4 */
					ist.ctlr_array[nmegaport].ctlr_rev = 0; 
				}
				/* Save model number only */
				else{
					mpd->mpd_id = bid & 0xF8;
					ist.ctlr_array[nmegaport].ctlr_rev = 
						bid & 0x7; /* revision */
				}
				ist.ctlr_array[nmegaport].ctlr_model = 
					mpd->mpd_id;
				ist.ctlr_array[nmegaport].ctlr_model_ext = 
					((mpd->mpd_id & 0xFF00) >> 8);
				mpd->mpd_bus = ISA_BUS;
				ist.ctlr_array[nmegaport].ctlr_bus_type = 
					ISA_BUS; /* ISA */
				mpd->mpd_io = k;

				mpd->mpd_board_def = find_board_def(
						mpd->mpd_id, ISA_BUS);

				if (mpd->mpd_board_def == NULL) {
					mpd->mpd_board_def = &unknown_board;
					ist.ctlr_array[nmegaport].cnfg_state = 2;
				} else
					ist.ctlr_array[nmegaport].cnfg_state = 1;

				printk("EQNX SST Board %d : id %4.4x (%s)\n",
					nmegaport,	
					mpd->mpd_id, mpd->mpd_board_def->name);
				if (mpd->mpd_board_def->asic == SSP64)
					/* SSP64 only */
					mpd->mpd_pgctrl = portn[k] + 2;
				else
					/* SSP2 or SSP4 */
					mpd->mpd_pgctrl = portn[k] + 8;
				ist.ctlr_array[nmegaport].ctlr_loc = portn[k];
				request_region(portn[k], 8, "eqnx");
				ist.ctlr_array[nmegaport].ctlr_mca_port = 0;
				pnp = 0; /* default */
				if ((mpd->mpd_board_def->asic == SSP4) &&
					(inb(portn[k] + 4) & 0x80)) { 
#ifdef	RELIABLE_PNP
					unsigned char port7,port9,porta;
#endif
					struct cntrl_cfig *idcp ;
					pnp = 1;
					/* use Plug and Play settings */ 
					idcp = &oper_input.isa_def_config;
					idcp->base_port = portn[k]; /* I/O port used */ 
	/*
	** Memory returned from PnP is unreliable - we'll find an ISA hole
	** ourselves.  The port number allocated is ok.
	**				Mikes 12/19/01
	*/
#ifdef	RELIABLE_PNP
					oper_input.isa_def_specd = 0xff; 
					idcp->cntrlr_mode = 0 ; /* Paged! */ 
					idcp->cntrlr_bus_inface = 0x20 ; /* 16 bits! */ 
					/* Address */ 
					port7 = inb(portn[k] + 0x7);
					port9 = inb(portn[k] + 0x9);
					porta = inb(portn[k] + 0xA);
					idcp->cntrlr_base_addr = (porta << 16) | ((port9 & 0xc0) << 8) ;
					/* Make sure Plug and Play gave us an address */
					if (!(idcp->cntrlr_base_addr 
						&& ((port7 & 0xC0) == 0xC0))) {
						ist.ctlr_array[nmegaport].cnfg_state=2; 
						ist.ctlr_array[nmegaport].cnfg_fail = 6;
						mpd->mpd_alive = false;
						nmegaport++;
		   			continue;
					}
#endif	/* RELIABLE_PNP */
				} /* end SSP4 PnP */ 
				if ((mpd->mpd_board_def->number_of_ports >= 64) &&
			    	((bid & 0x7) > 0) && ((inb(portn[k] + 2) & 0xC0) == 0xC0)) { 
#ifdef	RELIABLE_PNP
					unsigned char port4,port5,port7;
#endif
					struct cntrl_cfig *idcp ;
					pnp = 1;
					/* use Plug and Play settings */ 
					idcp = &oper_input.isa_def_config;
					/* I/O port used */ 
					idcp->base_port = portn[k]; 
	/*
	** Memory returned from PnP is unreliable - we'll find an ISA hole
	** ourselves.  The port number allocated is ok.
	**				Mikes 12/19/01
	*/
#ifdef	RELIABLE_PNP
					oper_input.isa_def_specd = 0xff; 
					idcp->cntrlr_mode = 0 ; /* Paged! */ 
					/* 16 bits! */ 
					idcp->cntrlr_bus_inface = 0x20 ; 
					/* Address */ 
					port4 = inb(portn[k] + 0x4);
					port5 = inb(portn[k] + 0x5);
					port7 = inb(portn[k] + 0x7);
					idcp->cntrlr_base_addr = 
					(port5 << 16) | ((port4 & 0xc0) << 8) ;
					if (!(idcp->cntrlr_base_addr && 
					((port7 & 0xC0) == 0xC0))) {
						ist.ctlr_array[nmegaport].
							cnfg_state=2; 
						ist.ctlr_array[nmegaport].
							cnfg_fail = 6;
						mpd->mpd_alive = false;
						nmegaport++;
						continue;
					}
#endif	/* RELIABLE_PNP */
				} /* end SSP64 PnP */ 
			if(!pnp) {
				if (MegAddr) {
					struct cntrl_cfig *idcp ;
					/* Default settings specified! */ 
					oper_input.isa_def_specd = 
						0xff; 
					idcp = &oper_input.isa_def_config;
					idcp->base_port = 
						portn[k]; /* I/O port used */ 
					/* Address */ 
					idcp->cntrlr_base_addr = MegAddr;
					idcp->cntrlr_mode = 0 ; /* Paged! */ 
					/* 16 bits! */ 
					idcp->cntrlr_bus_inface = 0x20 ;
				}
				else
					/* Dynamic for this board */ 
					oper_input.isa_def_specd = 1;

			}

#ifdef DEBUG
printk("before determ_isa_cnfg for address %x\n",(unsigned int) portn[k]);
#endif
/* Pat */
				determ_isa_cnfg(portn[k],&isa_parms,
					((mpd->mpd_board_def->asic == SSP64) ? 
					IFNS_SSP : IFNS_SSP4));
#ifdef DEBUG
printk("finished determ_isa_cnfg for address %x\n",(unsigned int) portn[k]);
#endif
/* Pat */
				ist.ctlr_array[nmegaport].cnfg_init = 
					isa_parms.cntrlr_parms.cntrlr_mode;

				if (isa_parms.cntrlr_parms.cntrlr_mode == 0x40) 
				{
					printk("Board in flat mode\n");
					/*  flat mode */
					mpd->mpd_nwin = 0;
					nports = mpd->mpd_board_def->number_of_ports;

					if (mpd->mpd_board_def->asic == SSP64) {
					/* SSP64 */
						if (nports == 128)
							mpd->mpd_addrsz = 
								FLAT128_MEM_LEN;
						else if (nports == 64)
							mpd->mpd_addrsz = 
								FLAT64_MEM_LEN;
						else
							mpd->mpd_addrsz = 
								FLAT8_MEM_LEN;
					} else {
					/* SSP2 / SSP4 */	
						/* don't follow - mikes */
						switch((bid & 0xFF00)>>8) {
						   case 0x24:
						   case 0x09:
						   case 0x36:
						      mpd->mpd_addrsz =
						      FLAT16K_MEM_LEN;
						      break;
						   case 0x1B:
						      mpd->mpd_addrsz =
						      FLAT32K_MEM_LEN;
						      break;
						   default:
						      mpd->mpd_addrsz =
						      FLAT16K_MEM_LEN;
						      break;
						}
					}
					/* FOR NOW FLAT MODE IS NOT ALLOWD FOR 
					 * ISA (default error case code) */
					ist.ctlr_array[nmegaport].cnfg_state=2; 
					/* invalid setting*/
					ist.ctlr_array[nmegaport].cnfg_fail = 6;
					mpd->mpd_alive = false;

					nmegaport++;
		   			continue; /* in the for loop */
				} else if (isa_parms.cntrlr_parms.cntrlr_mode ==
					0x0) {
					/*  use 16k window */
					mpd->mpd_nwin = HA_WIN16;
					/*  and 16K size */
					mpd->mpd_addrsz = 0x4000;
				} else {
					printk("Invalid card\n");
					ist.ctlr_array[nmegaport].cnfg_state=2; 
					/* invalid setting*/
					ist.ctlr_array[nmegaport].cnfg_fail = 6;
					mpd->mpd_alive = false;
					nmegaport++;
			   		continue;
				}

                                if (isa_parms.cntrlr_parms.cntrlr_bus_inface ==
					0x20)
					mpd->mpd_mem_width = 16;
				else {

					printk("EQNX: No 16 bit holes found\n");
  					/* walk all 8-bit holes run pram test */
                                        for (i=0; i< HOLE_TABLE_SIZE; i++)
                                        {
                                                if (ist.sys_isa_holes[i] == 0)
                                                       continue;
                                                if (ist.sys_isa_hole_status[i]==
							0xf1 ||
						   ist.sys_isa_hole_status[i] ==
							0xfe)
                                                {
							b0 = test_8bit_hole(mpd,k,
							ist.sys_isa_holes[i]);
							if (b0)
								{
                                                              printk("EQNX: Found a hole at %d \n",i);
                                                              break;
                                                           }
                                                 }
                                        } /* End for */
                                        if (b0)
                                        { 
						mpd->mpd_mem_width = 16;
                                                isa_parms.cntrlr_parms.cntrlr_base_addr =
                                                     ist.sys_isa_holes[i];
                                                printk("EQNX: ISA 8bit Address %x as 16 bit wide hole.\n",
                                                     (unsigned int) ist.sys_isa_holes[i]);
					} /* End if(b0) */

					else {
						/* 
						 * End 0xb0000 8-bit kludge code
						 */
						mpd->mpd_mem_width = 8;
						/* FOR NOW 8 bit MODE IS NOT 
						 * ALLOWD FOR ISA (default 
						 * error case code) */
						printk("EQNX:ISA: No 16 bit wide holes found.\n");
						ist.ctlr_array[nmegaport].cnfg_state = 2; 
						ist.ctlr_array[nmegaport].cnfg_fail = 6; /* invalid setting*/
                                                mpd->mpd_alive = false;
                                                nmegaport++;
		   				continue; /* in the for loop */
					}
				} /* else */

				mpd->mpd_pmem = 
					isa_parms.cntrlr_parms.cntrlr_base_addr;
#ifdef DEBUG
printk("ISA board mem addrress is %x\n", (unsigned int) mpd->mpd_pmem);
#endif
				mpd->mpd_nicps = 
					mpd->mpd_board_def->number_of_asics; 
				nicps += mpd->mpd_nicps;
				mpd->mpd_nchan = 
					mpd->mpd_board_def->number_of_ports;
                                /*SSM, found while testinf SSM, no memsz of ISA
                                    brds, thus no memory tests .....
                                */
				nports = mpd->mpd_board_def->number_of_ports;
				if (mpd->mpd_board_def->asic == SSP64) {
				/* SSP64 */
					if (nports == 128)
                                         	mpd->mpd_memsz= 0x100000;
						/* 1 meg */
					else if (nports == 64)
                                         	mpd->mpd_memsz= 0x80000;
						/* 0.5 meg */
					else
                                        	mpd->mpd_memsz= 0x8000;
						/* 32k */
				} else {
				/* SSP2 / SSP4 */
					if (nports <= 4)
				        	mpd->mpd_memsz = 0x4000;
						/* 16K */
					else if (nports == 8)
				        	mpd->mpd_memsz = 0x8000;
						/* 32K */
					else
						mpd->mpd_memsz = 0x10000; 
						/* 64K */
                                }
                                mpd->mpd_minor = nextmin;
				nextmin += NCHAN_BRD;
				mpd->mpd_alive = true;
				mpd->mpd_slot = 0;
				nmegaport++;

    				if (mpd->mpd_board_def->asic == SSP64) { 
					/* SSP64 board */
       					/* Set the address, 16-bit mode, MAX2, 
					 * and flat off */ 
       					outb(( (mpd->mpd_pmem >> 14) & 0xff),
						portn[k]);
       					outb(((mpd->mpd_pmem >> 22) & 3)| 0x20,
						portn[k] + 1);
       					/* page register - deselect ram */  
				
       					outb(0,portn[k] + 2);
       					outb(0,portn[k] + 3);

       					/* disable interrupts */ 
       					outb(0, portn[k] + 6);
    				}
    				else { /* SSP4 board */
      					unsigned char b;
    				 
      					/* reg 8 (page reg) */
      					if(mpd->mpd_nwin == HA_FLAT)
         					b = 0x20;
      					else
         					b = 0;

      					outb( b | 0x00, portn[k] + 0x08);

      					b = ((mpd->mpd_pmem & 0x0000c000) >> 8);
      					/* no ints */
      					outb(b, portn[k] + 0x09);
      					b =((mpd->mpd_pmem & 0xFFFF0000) >> 16);
      					outb(b,portn[k] + 0x0a);
      					/* reg b*/
      					b=0;
      					outb(b,portn[k] + 0x0b);
    				} /* SSP4 */
	
			} /* board id found */
		} /* ISA for loop */
	}
#endif	/* ISA_ENAB || MCA_ENAB */

#ifdef	MCA_ENAB
	if (mca_bus) {
 		/* loop thru MCA slots to board(s) */
		for ( k = 0; k < 8; k++ )
		{
			switch ( mca_brd_found( k, mca_cfg ) )
		    {
		    case true:
				if (++ist.ctlr_found > maxbrd) {
					ist.ctlr_array[nmegaport].cnfg_state = 2; 
					ist.ctlr_array[nmegaport].cnfg_fail = 5; /* maxbrd exceeded */
		   		continue;
				}
				else
					ist.ctlr_array[nmegaport].cnfg_state = 1; 
				mpd = &meg_dev[nmegaport];
				mpd->mpd_alive = true;
				mpd->mpd_slot = k;
				ist.ctlr_array[nmegaport].ctlr_loc = mpd->mpd_slot;
				mpd->mpd_id = mca_cfg[8] & 0xf8; /* Save model number only */
				ist.ctlr_array[nmegaport].ctlr_model = mpd->mpd_id;
				ist.ctlr_array[nmegaport].ctlr_model_ext = 
					((mpd->mpd_id & 0xFF00) >> 8);
				ist.ctlr_array[nmegaport].ctlr_rev = mca_cfg[8] & 0x7;/* revision */
				mpd->mpd_bus = MCA_BUS;
				ist.ctlr_array[nmegaport].ctlr_bus_type = MCA_BUS; 
				ist.ctlr_array[nmegaport].ctlr_mca_port = mca_cfg[10];
				ist.ctlr_array[nmegaport].cnfg_init = 1; /* MCA is autoconfig */

				mpd->mpd_board_def = find_board_def(
						mpd->mpd_id, MCA_BUS);

				if (mpd->mpd_board_def == NULL)
					mpd->mpd_board_def = &unknown_board;

				printk("EQNX SST Board %d : id %4.4x (%s)\n",
					nmegaport,	
					mpd->mpd_id, mpd->mpd_board_def->name);
				mpd->mpd_nchan = 
					mpd->mpd_board_def->number_of_ports;
				mpd->mpd_minor = nextmin;
				nextmin += NCHAN_BRD;
				mpd->mpd_nicps = 
					mpd->mpd_board_def->number_of_asics;

				nicps += mpd->mpd_nicps;
				mpd->mpd_pmem = mca_base_paddr( mca_cfg );
				ist.ctlr_array[nmegaport].cnfg_physc_base = mpd->mpd_pmem;
                                if ((mpd->mpd_addrsz = mca_mem_size( mca_cfg, 0 ))==0)
                                {
                                    ist.ctlr_array[nmegaport].cnfg_state = 2;
                                    ist.ctlr_array[nmegaport].cnfg_fail = 6; /* fail bad address */
                                    nextmin += NCHAN_BRD;
                                    nmegaport++;
                                    mpd->mpd_alive = false;
                                    continue;
                                }
                                ist.ctlr_array[nmegaport].cnfg_addr_size = mpd->mpd_addrsz;
				if ((mpd->mpd_nwin = mca_mem_strat(mca_cfg)) == HA_WIN16)
					ist.ctlr_array[nmegaport].cnfg_mode = 1 /* Paged */;
				else
					ist.ctlr_array[nmegaport].cnfg_mode = 2 /* Flat */;
				mpd->mpd_mem_width = 16;
				ist.ctlr_array[nmegaport].cnfg_isa_bus_sz = 0x10; /* 16 bit mode */
				mpd->mpd_memsz = mca_mem_size( mca_cfg, 1 );
				mpd->mpd_pgctrl = (mca_cfg[11] << 8) | (mca_cfg[10] + 2);
				nmegaport++;
					break;
		    case -1:
		printk("Run Microchannel configuration utility for Equinox board in slot %d.\n", 
		     	k );
					break;
				default:
					break;
    		}
			} /* for */
		} /* mca_bus */
#endif	/* MCA_ENAB */

	if(!nmegaport) {
	   printk("NOTICE: No Equinox boards found.\n");
	   return 1;
	}
	if((meg_chan = (struct mpchan *)vmalloc(sizeof(struct mpchan) * 
			nmegaport * NCHAN_BRD)) == (struct mpchan *) NULL){
		printk("EQUINOX: Failed to allocate virtual address space of size for meg_chan %ld\n", (unsigned long)(sizeof(struct mpchan) * nmegaport * NCHAN_BRD));
		return(-1);
		}
	memset(meg_chan, 0, (sizeof(struct mpchan) 
			* nmegaport * NCHAN_BRD));
	mpc = meg_chan;
	for(k=0; k < nmegaport; k++) {
		/* map Adapter memory */
		mpd = &meg_dev[k];
		if (mpd->mpd_alive == 0) 
			continue; /* If board is dead skip it */
		mpd->mpd_mpc = (struct mpchan *)&meg_chan[k * NCHAN_BRD];
/* Pat */	
#if (EQNX_VERSION_CODE < 131328)
	if (mpd->mpd_pmem > 0x100000){
#endif
/* Pat */
		if((mpd->mpd_mem = (paddr_t) ioremap(mpd->mpd_pmem,mpd->mpd_addrsz)) == (paddr_t) NULL)
		{
			printk("EQUINOX: Failed to allocate virtual address space of size %d\n",
				mpd->mpd_addrsz);
		}
/* Pat */	
#if (EQNX_VERSION_CODE < 131328)
	}
	else
		mpd->mpd_mem =  mpd->mpd_pmem;
#endif
/* Pat */  
		if(!mpd->mpd_mem) {
	   		printk("EQNX: Memory mapping failed for board %d\n", 
				k +1);
			/* Driver init failed */
			ist.ctlr_array[k].cnfg_state = 2; 
			ist.ctlr_array[k].cnfg_fail = 1; /* sptalloc failed */
                	mpd->mpd_alive = false;
        	   	continue;
		}
        	brd_mem_cfg(mpd);
		hwq = mpd->mpd_hwq;

		/* enable board memory - for eisa, memory is switched off at 
		 * boot */
		/* However, for PCI, memory is switched on already. */
		if((mpd->mpd_nwin == HA_FLAT) && !(mpd->mpd_bus == PCI_BUS)) {
                  if(mpd->mpd_board_def->asic == SSP64) /* SSP64 only */
	   	    outw(0x100, mpd->mpd_pgctrl); /* enable all in flat mode */
                  else
                     /*SSP2 enables all for HA_FLAT */
                     outb( 0x20, mpd->mpd_pgctrl);  
                } /* HA_FLAT */

		if(mpd->mpd_board_def->asic == SSP64) 
			mpd->mpd_sspchan = NCHAN; /* SSP64 */
		else
			mpd->mpd_sspchan = 4; /* SSP4 */ 

		/* loop n icps */
		for(j=0; j < (int)mpd->mpd_nicps; j++) {
			int mux;

			/* select correct page if necessary */
			if(mpd->mpd_nwin == HA_WIN16) {
                          if(mpd->mpd_board_def->asic == SSP64)
			    outw(0x100 | j ,mpd->mpd_pgctrl);
                          else
                            outb(1 << j,  mpd->mpd_pgctrl); 
                        } /* HA_WIN16 */

			/* reset icp to known state */
			icp = &mpd->icp[j];
			icp->icp_minor_start = mpd->mpd_minor + (j * mpd->mpd_sspchan);
                        if (mpd->mpd_board_def->asic == SSP64) {  /* SSP64 */
			  icpg = (icpgaddr_t)((unsigned long)icp->icp_regs_start+0x2000);
			  i_gicp_attn = 0;
			  Gicp_initiate = 0;
                          bus_ctrl_p = &(icpg->ssp.gicp_bus_ctrl);
			/*
	 		 * If on pci_bus, then we must set up the Global Bus 
			 * Control Register with the CPU bus width (32 bit bus)
			 * and the 40 Bit DRAM bit. The Global Bus Control 
			 * Register is at offset 0x18.
	 		 */
	   		if(!j) {
	      			if ( mpd->mpd_bus ==  PCI_BUS)
					*bus_ctrl_p = 
						(STERNG | DRAM_40 | CPU_BUS_32);
	      			else
					/* 16-bits|dram-steer */
	         			*bus_ctrl_p = 0x9;
	   		}
	   		else {
	      			if ( mpd->mpd_bus ==  PCI_BUS)
	         			*bus_ctrl_p = (DRAM_40 | CPU_BUS_32);
	      			else
	         			*bus_ctrl_p = 0x1;		/* 16-bits */
	   		}
                        }
                        else { /* SSP2/4 */
                          /* VIRTUAL- ptr to global regs */
                          volatile union global_regs_u *icp_glo =
                            (volatile union global_regs_u *)((unsigned long)icp->icp_regs_start + 0x400);
                          /* VIRTUAL ptr to output regs */
                          volatile union output_regs_u *icp_cout =
                            (volatile union output_regs_u *)((unsigned long)icp->icp_regs_start + 0x200);
                          /* VIRTUAL ptr to input regs */
                          volatile union input_regs_u *icp_cin =
                            (volatile union input_regs_u *)(icp->icp_regs_start);

	                  icp_glo->ssp2.on_line = 0;   /* disable SSP2 */
	                  icp_cin->ssp2.locks = 0xFF;/* input locks SSP2 */
                          /* output locks SSP2 */
	                  icp_cout->ssp2.locks = 0x77;
                          /* Set Bus Control = 16 bits */
                          bus_ctrl_p = &(icp_glo->ssp2.bus_cntrl);
                          *bus_ctrl_p = 0x05;

			  if (mpd->mpd_board_def->number_of_ports == 2)
			  	ssp_channels = 2;
			  else	ssp_channels = 4;

                          for(ii = 0; ii < ssp_channels; ii++) /* set all locks */
                          {
                              ((volatile union input_regs_u *)((char *)icp_cin + ii))->ssp2.locks = 0xFF;
                              ((volatile union output_regs_u *)((char *)icp_cout + ii))->ssp2.locks = 0x77;
                              ((volatile union output_regs_u *)((char *)icp_cout + ii))->ssp2.cntrl_sigs = 0x0F;
                          }
			  icp_glo->ssp2.bus_cntrl = 0x01;
			  icp_glo->ssp2.on_line = 0;
                        }
	
			/* Adjust the number of icps on the board  */
			/*
			printk("no of icps found = %d\n",mpd->mpd_nicps);
			*/

  			if( (ii = mem_test(mpd,j))) {
				printk("EQNX: Memory test failed for board %d SSP %d\n",
				k+1,j+1);
				ist.ctlr_array[k].cnfg_state = 2; /* Driver init failed */
				ist.ctlr_array[k].cnfg_fail = 2; /* mem_test failed */
                		mpd->mpd_alive = false;
                		continue;
  			}

  			/* verify that pram is not cached */
    			if (mpd->mpd_board_def->asic == SSP64) { 
				/* SSP64 board */
			  	icpg = (icpgaddr_t)((unsigned long)icp->icp_regs_start+0x2000);
       				/* verify that pram is not cached */
      				cchnl = Gicp_chnl;
      				chnl_ptr = &(Gicp_chnl);
    			}
    			else { /* SSP4 board */
      				icpaddr_t icp;
      				volatile union global_regs_u *icp_glo ;
      				icp = &mpd->icp[j];
      				icp_glo = (volatile union global_regs_u *)((unsigned long)icp->icp_regs_start + 0x400);
      				cchnl = icp_glo->ssp2.chan_ctr;
      				chnl_ptr = &(icp_glo->ssp2.chan_ctr);
    			}
      			no_cache = false;
      			for ( ii = 0; ii < 0x100000; ii++ )
        			if ( *chnl_ptr != cchnl )
        			{
          				no_cache = true;
          				break;
        			}
  			if ( !no_cache)
  			{
    				if (mpd->mpd_bus == ISA_BUS)
          			printk("EQNX: PRAM memory appears to be cached %lx for I/O address 0x%x.\n",
          			(paddr_t) mpd->mpd_mem,portn[mpd->mpd_io]);
    				else
          				printk("EQNX: PRAM memory appears to be cached %lx for board in Slot %d.\n",
          				(paddr_t) mpd->mpd_mem,mpd->mpd_slot);
				/* Driver init failed */
				ist.ctlr_array[k].cnfg_state = 2; 
				/* PRAM cached */
				ist.ctlr_array[k].cnfg_fail = 3; 
                		mpd->mpd_alive = false;
                		continue;
  			} /* End if(!no_cache) */
    			lmx = 0;
    			mp = (mpaddr_t)mpd->icp[j].icp_regs_start;

      			if(mpd->mpd_board_def->asic == SSP64) {
				if (mpd->mpd_board_def->number_of_asics > 0)
					/* Number per ICP */
					ssp_channels = 64;
				else	ssp_channels = 0;
      			}
#ifdef DEBUG
printk("eqnx_init:queue size for board %d = %d\n\n", k, mpd->mpd_hwq->hwq_size);
#endif

/*NEW 3.30*/
    			if (mpd->mpd_board_def->asic != SSP64) {/* SSP4 board */
      				icpaddr_t icp;
      				volatile union global_regs_u *icp_glo ;
      				icp = &mpd->icp[j];
      				icp_glo = (volatile union global_regs_u *)((unsigned long)icp->icp_regs_start + 0x400);
      				cchnl = icp_glo->ssp2.chan_ctr;
      				chnl_ptr = &(icp_glo->ssp2.chan_ctr);
						for ( ii = 0; ii < 0x10000; ii++ )
							if ( *chnl_ptr != cchnl ) break;
							icp_glo->ssp2.bus_cntrl = 0xCD; 
						cchnl = icp_glo->ssp2.chan_ctr;
						for ( ii = 0; ii < 0x10000; ii++ )
							if ( *chnl_ptr != cchnl ) break;
    			}
/*NEW 3.30*/
			mux = 0; /* if SST-16I or SST-16P must mux ctrl sigs */
      			if (mpd->mpd_board_def->asic != SSP64) { 
				if (mpd->mpd_board_def->number_of_ports == 16)
					mux = 1;
			}

  			/*loop thru each channel - all bytes have been zero'd */
  			for ( i = 0; i <  ssp_channels; i++ )
			{
    				/* setup mpc virtual addresses for cpu access 
				 * of icp */
				jj = mpd->mpd_minor + i + 
					(j * mpd->mpd_sspchan);
				mpc = &meg_chan[jj];
				mpc->mpc_brdno = k;
				/* chan # on this icp */
				mpc->mpc_chan = i;
				mpc->mpc_icpi = (icpiaddr_t)&mp->mp_icpi[i];
      				if(mpd->mpd_board_def->asic == SSP64)
					mpc->mpc_icpo = (icpoaddr_t)&mp->mp_icpo[i];
				else
					mpc->mpc_icpo = (icpoaddr_t)((unsigned char *)(mpc->mpc_icpi) + 0x200);
				mpc->mpc_mpd = mpd;
				mpc->mpc_icp = (icpaddr_t)&mpd->icp[j];
				/* icp # on this board */
				mpc->mpc_icpno = j;
				mpc->mpc_ptty = mpc->mpc_tty;


				if((mpc->normaltermios = 
				(struct termios *)vmalloc(sizeof(
				struct termios))) == (struct termios *) NULL){
					printk("EQUINOX: Failed to allocate virtual address space of size %d for normaltermios\n", (unsigned int)sizeof(struct termios));
					return(-1);
				}
				memset(mpc->normaltermios, 0, 
					sizeof(struct termios));
				*mpc->normaltermios = eqnx_deftermios;
#if	(LINUX_VERSION_CODE < 132096)
				if((mpc->callouttermios = 
				(struct termios *)vmalloc(sizeof(
				struct termios))) == (struct termios *) NULL){
					printk("EQUINOX: Failed to allocate virtual address space of size %d for callouttermios\n", (unsigned int)sizeof(struct termios));
					return(-1);
				}
				memset(mpc->callouttermios, 0, 
					sizeof(struct termios));
				*mpc->callouttermios = eqnx_deftermios;
#endif
				mpc->closing_wait = CLSTIMEO;
				mpc->close_delay = EQNX_CLOSEDELAY;
#if	(LINUX_VERSION_CODE < 132608)
				/* 2.2. and 2.4 kernels */
				mpc->tqhangup.routine = eqnx_dohangup;
#else
				/* 2.6+ kernels */
				mpc->tqhangup.func = eqnx_dohangup;
#endif
				mpc->tqhangup.data = mpc;

				/*
				** initialize each of the wait queues
				*/
#if	(LINUX_VERSION_CODE < 132096)
				/* kernels before 2.4 */
				init_waitqueue(&mpc->open_wait);
				init_waitqueue(&mpc->close_wait);
				init_waitqueue(&mpc->raw_wait);
				init_waitqueue(&mpc->mpc_mpa_slb);
				init_waitqueue(&mpc->mpc_mpa_clb);
				init_waitqueue(&mpc->mpc_mpa_rst);
				init_waitqueue(&mpc->mpc_mpa_call_back);
#else
				/* 2.4 kernels and after */
				mpc->open_wait_wait = 0;
				mpc->mpc_mpa_slb_wait = 0;
				mpc->mpc_mpa_clb_wait = 0;
				mpc->mpc_mpa_rst_wait = 0;
				mpc->mpc_mpa_call_back_wait = 0;
				init_waitqueue_head(&mpc->open_wait);
				init_waitqueue_head(&mpc->close_wait);
				init_waitqueue_head(&mpc->raw_wait);
				init_waitqueue_head(&mpc->mpc_mpa_slb);
				init_waitqueue_head(&mpc->mpc_mpa_clb);
				init_waitqueue_head(&mpc->mpc_mpa_rst);
				init_waitqueue_head(&mpc->mpc_mpa_call_back);
#endif

				mpc->mpc_last_esc = 0xffff;
				mpc->mpc_esc_timo = 0;
				mpc->mpc_scrn_no = 0;
				mpc->mpc_input = 0;
				mpc->mpc_output = 0;
				mpc->mpc_scrn_count = 0;
				mpc->mpc_cin_events = 0;
				mpc->mpc_cout_events = 0;
				mpc->mpc_cin_ena = 0;
				mpc->mpc_cout_ena = 0;
				mpc->mpc_mpa_stat = 0;
         			mpc->mpc_mpa_init_retry = 0;
         			mpc->mpc_mpa_reset_retry = 0;

				mpc->mpc_parity_err_cnt = 0;
				mpc->mpc_framing_err_cnt = 0;
				mpc->mpc_break_cnt = 0;

        			icpi = mpc->mpc_icpi;
				icpo = mpc->mpc_icpo;
      				if(mpd->mpd_board_def->asic == SSP64) {
					icpg = (icpgaddr_t)icpo;
					lmx_factor = 16; 
				}
				else {
					icpg = (icpgaddr_t)((unsigned long)icpi + 0x400);
					lmx_factor = 4; 
				}
				icpq = &icpo->ssp.cout_q0;
				if(i == 0 * lmx_factor) {
	  				lmx = 0; 
	  				mpc->mpc_icp->lmx[lmx].lmx_mpc = mpc;
	  				mpc->mpc_icp->lmx[lmx].lmx_wait = -1;
				}

       				if(i == 1 * lmx_factor) {
         				lmx = 1;
         				mpc->mpc_icp->lmx[lmx].lmx_mpc = mpc;
         				mpc->mpc_icp->lmx[lmx].lmx_wait = -1;
      				}

        			if(i == 2 * lmx_factor) {
	   				lmx = 2;
	   				mpc->mpc_icp->lmx[lmx].lmx_mpc = mpc;
	   				mpc->mpc_icp->lmx[lmx].lmx_wait = -1;
				}
				if(i == 3 * lmx_factor) {
	   				lmx = 3; 
	   				mpc->mpc_icp->lmx[lmx].lmx_mpc = mpc;
	   				mpc->mpc_icp->lmx[lmx].lmx_wait = -1;
				}
				mpc->mpc_lmxno = lmx;


    				/* icp input - assign queues, etc. */
    				/* input buffer base pointer also sets tag base pointer! */
    				/* setup input hardware registers */
/*NEW*/
				icpi->ssp.cin_locks = 0xff; /* set locks */
				icpo->ssp.cout_lock_ctrl = 0xff;
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				cur_chnl_sync(mpc); /* mpc is sufficiently initialized */
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
/*NEW*/
				if(mpd->mpd_board_def->asic == SSP64) {
					/* SSP64 */
    					addr = (i + j * NCHAN) * 2 * hwq->hwq_size;
    					icpi->ssp.cin_dma_base = (addr >> 16);
    					icpi->ssp.cin_bank_a.ssp.bank_dma_ptr
          				= icpi->ssp.cin_bank_b.ssp.bank_dma_ptr
          				= icpi->ssp.cin_tail_ptr_a 
          				= icpi->ssp.cin_tail_ptr_b 
          				= addr & 0xffff;
        			} else {
					/* SSP2 / SSP4 */
           				addr = i * 0x400;  
           				icpi->ssp2.bank_a.nxt_dma
              				= icpi->ssp2.bank_b.nxt_dma
              				= icpi->ssp.cin_tail_ptr_a 
              				= icpi->ssp.cin_tail_ptr_b  
              				= addr & 0xffff;
        			}

    				/* setup mpc values for input hardware registers */
   				if(mpd->mpd_nwin == HA_FLAT) {
     					mpc->mpc_rxq.q_begin = addr & 0xffff;
     					mpc->mpc_rxq.q_ptr = addr & 0xffff;
     					mpc->mpc_rxbase = 0;
     					mpc->mpc_rxpg = 0;
     					mpc->mpc_tgpg = 0;
     					mpc->mpc_rxq.q_end =  (addr & 0xffff) + (hwq->hwq_size - 1); 
     					mpc->mpc_rxq.q_size = hwq->hwq_size;
					if (mpd->mpd_board_def->asic == SSP64) {
     						mpc->mpc_tags = addr >> 4;
     						mpc->mpc_rxq.q_addr = 
						(char *)(icp->icp_dram_start + 
						((i * 2 * hwq->hwq_size) & 
						0xffff0000));
					}
					else {
     						mpc->mpc_tags = 0;
     						mpc->mpc_rxq.q_addr = 
						(char *)(icp->icp_dram_start + 
						((i * hwq->hwq_size) & 
						0xffff0000));
					}
   				} else { 
     					mpc->mpc_rxq.q_begin = addr % 0x4000;
     					mpc->mpc_rxq.q_ptr = addr % 0x4000;
     					mpc->mpc_rxpg = addr / 0x4000;
     					mpc->mpc_rxbase = addr & 0xc000;
     					mpc->mpc_tags = (addr >> 4) / 0x4000;
     					mpc->mpc_tgpg = (addr >> 2) / 0x4000;
     					mpc->mpc_rxq.q_end =  (addr % 0x4000) + (hwq->hwq_size - 1); 
     					mpc->mpc_rxq.q_size = hwq->hwq_size;
     					mpc->mpc_rxq.q_addr = (char *)(icp->icp_dram_start);
   				}
    				/* setup additional icp input registers */
    				icpi->ssp.cin_overload_lvl = hwq->hwq_hiwat;
    				icpi->ssp.cin_susp_output_lmx = 0x41; 
				/* susp on lmx problems
				     	- TEMP was 0x63
                                     	- leave sigs masked */
    				icpi->ssp.cin_susp_output_sig = 0x00;
    				icpi->ssp.cin_q_ctrl = 0x20 | hwq->hwq_rxwrap;
				/* input q wrap size, tail ptr a, 
				 * overload enabled */
    				icpi->ssp.cin_min_char_lvl = 1;
    				icpi->ssp.cin_inband_flow_ctrl = 0;
    				/* unlock input */
				/* Start with Bank B locked */
    				icpi->ssp.cin_locks = 0x10 | LOCK_B; 
	
    				/* icp output - assign data queues and 
				 * command queues */
				/* setup queue 0 only for each channel */
    				/* use circular output data queue 
				 * - no session registers */

				if (mpd->mpd_board_def->asic == SSP64) {
					addr =((i + j * NCHAN) * 2 + 1) * 
					hwq->hwq_size;
    				Q_data_base = (addr >> 16);
    				Q_data_ptr = (addr & 0xffff);
    				q_type = 
			       		EN_CIRC_Q|EN_TX_LOW|EN_TX_EMPTY | 
					hwq->hwq_txwrap; 
    					/* permanent send data state */
    					Q_block_count = hwq->hwq_lowat / 64;  
					/* lowat mark in 64-byte blocks */ 
    					/* assign session reg. as first within 
			 	 	* this channel block */
    					icpo->ssp.cout_status |= (i << 10);
    					/* those fields need to be defined */
				} else {
					addr = i * 0x400;
					icpo->ssp2.q0.data_ptr = addr & 0xFFFF;
					icpo->ssp2.q0.buff_type_sz =
			       		   EN_CIRC_Q|EN_TX_LOW|EN_TX_EMPTY | 
					   hwq->hwq_txwrap; 
					icpo->ssp2.q0.low_threshold = 
					   hwq->hwq_lowat / 64;
					icpo->ssp2.intern_status |=
					   ((i * 4) << 8);
				}

				if(mpd->mpd_board_def->asic == SSP64) {
					/* circular,size, cause empty & lowat 
					 * events */
    					Q_out_state = CMDQ_CONT_SND | 
						hwq->hwq_cmdsize; 
    					icpo->ssp.cout_ses_ctrl_a = 1;
        			} else {
           				icpo->ssp2.q0.send_data_state = 0x10;  
           				icpo->ssp2.ses_a.expnd_ena = 0x01;
        			}

   				if(mpd->mpd_nwin == HA_FLAT) {
					int addr1;
					if (mpd->mpd_board_def->asic != SSP64)
						addr1 = addr + 0x1000;
					else
						addr1 = addr;
     			 		mpc->mpc_txq.q_begin = addr1 & 0xffff;
     			 		mpc->mpc_txq.q_ptr = addr1 & 0xffff;
     					mpc->mpc_txbase = 0;
     					mpc->mpc_txpg = 0;
     					mpc->mpc_txq.q_end =  
						(addr1 & 0xffff) + 
						(hwq->hwq_size - 1); 
     					mpc->mpc_txq.q_size = hwq->hwq_size;
					if (mpd->mpd_board_def->asic == SSP64) 
     						mpc->mpc_txq.q_addr = 
						(char *)(icp->icp_dram_start + 
						(((i * 2 + 1) * hwq->hwq_size) &
						0xffff0000));
					else
     						mpc->mpc_txq.q_addr = 
						(char *)((unsigned long)icp->icp_dram_start + 
						((i * hwq->hwq_size) & 0xFFFF0000));
   				} else {
     					mpc->mpc_txq.q_begin = addr % 0x4000;
     					mpc->mpc_txq.q_ptr = addr % 0x4000;
     					mpc->mpc_txbase = addr & 0xc000;
     					mpc->mpc_txpg = addr / 0x4000;
     					mpc->mpc_txq.q_end =  
						(addr % 0x4000) + 
						(hwq->hwq_size - 1); 
     					mpc->mpc_txq.q_size = hwq->hwq_size;
					if (mpd->mpd_board_def->asic == SSP64) 
     						mpc->mpc_txq.q_addr = 
						(char *)(icp->icp_dram_start);
					else
     						mpc->mpc_txq.q_addr = 
						(char *)(icp->icp_dram_start +
						0x1000);
   				}
    				/* cmd queue */
				if (mpd->mpd_board_def->asic == SSP64)
    					addr = (MAXICPCHNL + i + 
					(j * NCHAN)) * hwq->hwq_size / 4;
				else
					addr = i * 0x400;
    				Q_cmd_base = (addr >> 16);
    				Q_cmd_ptr = (addr & 0xffff);

    				/* output timer - setup for 10 ms prescale */
    				icpo->ssp.cout_tmr_size = 2;   /* never change */
				/* place ms count here */
    				icpo->ssp.cout_tmr_count = 0;  

    				/* misc. output registers */
    				icpo->ssp.cout_ctrl_sigs &= ~0xff;  /* all off */
					if (mux)
	    				icpo->ssp.cout_ctrl_sigs |= 0x44;  /* mux em */
    				/* unlock output */
    				icpo->ssp.cout_lock_ctrl = 0x02;
    				icpo->ssp.cout_cpu_req ^= 0x04;  
				/* force send data state */
			}
  			/* check sanity of icp */
  			/* "ring clock failure" should be on since ring clock 
			 * is off */
			if (mpd->mpd_board_def->asic == SSP64) {
				/* SSP64 */
  				no_icp = !( i_gicp_attn & 0x04 );
  				if ( !no_icp || EQNft )
  				{
     					mpc = &meg_chan[icp->icp_minor_start];
    					/* setup mpc structures for each 
					 * channel */
    					for ( ii = 0; ii < ssp_channels; ii++,
						mpc++ )
    					{
      						/* don't enable lmx_cond_chng 
						 * until ring found */
      						icpi = mpc->mpc_icpi;
      						icpo = mpc->mpc_icpo;
						/* allow software attn */
      						icpi->ssp.cin_attn_ena = 0x8000;
						if (!(ii % 16))
							icpi->ssp.cin_attn_ena |= ENA_LMX_CNG;
						/* start with Bank B locked */
      						icpi->ssp.cin_locks = LOCK_B;
						/* allow software attn */
      						icpo->ssp.cout_attn_ena=0x8000;
      						icpo->ssp.cout_lock_ctrl^=0x03;
    					}

    					/* setup global interval timer for a 1 
					 * second pulse */
					/* 0.1 seconds before decrement */
    					Gicp_tmr_size = 192;   
					/* 10 decrements before pulse */
    					Gicp_tmr_count = 10;   
					gicp_bus |= 0x40;   /* enable it */

	icpg->ssp.gicp_watchdog = 0; /* Disable watchdog until megapoll runs */
       	 
    					/* enable global pram writes, dma, and 
				 	 * ring clock */
    					/* remaining ring/lmx managment 
					 * performed by megapoll() */
    					if(EQNft) {
	    					/* On fault tolerant system, 
					 	 * don't start up the ring clock
               				 	 * until program gives proper 
						 * IOCTL to turn it on. */
	    					Gicp_initiate = 
							0x1e - RNG_CLK_ON;
    					} else {
    						Gicp_initiate = 0x1e;
					}
    					mpd->icp[j].icp_rng_state = RNG_BAD;
					/* set ring bad */
    					mpd->icp[j].icp_rng_last = 0x4; 
  				} /* End if(!no_icp | EQNft) */
  				else
  				{
                			printk("EQNX: SSP %d not detected for board with I/O address%d.\n",j+1,k+1);
					/* Driver init failed */
					ist.ctlr_array[k].cnfg_state = 2; 
					/* ICP failed */
					ist.ctlr_array[k].cnfg_fail = 4; 
                			mpd->mpd_alive = false;
                			continue;
  				}
/* RAMP START ---------------------------------------------- */
        /* If it is an expandable board 64 or 128 port allocate memory
	   and  register it with the RAMP services software  */

	  	if((mpd->mpd_board_def->number_of_ports >= 64)
   	    		&& (mpd->mpd_alive == DEV_GOOD)) 
		{
                /*icpgaddr_t icpg;*/
		int retv;
                int index = (k * NSSPS) + j;
     		mpc = &meg_chan[icp->icp_minor_start];
		icp->sspcb = &ssp_dev[index];
    		icp->slot_ctrl_blk = &slot_dev[index * MAXICPCHNL];
                if ((retv = ramp_id_functs())) {
                   MESSAGE("ERROR id_functs:", retv, mpd->mpd_mpc);
    		   mpd->mpd_alive = DEV_BAD;
		   break;
                }
		    icp->sspcb->in_use = 0;
		    icp->sspcb->signature = NULL;

                    icpg = (icpgaddr_t)((unsigned long)icp->icp_regs_start+0x2000);
    	      	    if( mpd->mpd_nwin == HA_WIN16 ) {
			spin_lock_irqsave(&mpd->mpd_lock, flags);
                        mega_push_win(mpc, 0); 
		       /* register it */
	       	       retv = mpa_srvc_reg_ssp(icp->sspcb, 
		   	   (struct mpa_global_s *)icpg,
                           ramp_map_fn, 
                           mpc, 
                           ramp_unmap_fn);
                        mega_pop_win(43); 
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
                    }  else
                       retv = mpa_srvc_reg_ssp(icp->sspcb,
                           (struct mpa_global_s *)icpg,
                           NULL,NULL,NULL);
 		    if(retv)  {
               MESSAGE("ERROR reg_ssp:", retv, mpd->mpd_mpc);
    		   	mpd->mpd_alive = DEV_BAD;
			break;
                    }
                    if ((retv = mpa_srvc_pnp_probe_cntrl(icp->sspcb, 0xFF)))
                       MESSAGE("ERROR pnp_probe:", retv, mpd->mpd_mpc);
        }
/* RAMP END ------------------------------------------------ */
			} else { /* SSP2 board */
      				volatile union global_regs_u *icp_glo ;
      				icp_glo = (volatile union global_regs_u *)((unsigned long)icp->icp_regs_start + 0x400);
     				mpc = &meg_chan[icp->icp_minor_start];
				for (jj = 0; jj < ssp_channels; jj++,mpc++) {
					/* same for both FLAT and PAGED */
      					icpi = mpc->mpc_icpi;
      					icpo = mpc->mpc_icpo;
					icpi->ssp.cin_q_ctrl |= 0x80;
					/* allow software attention */
					icpi->ssp2.attn_ena = 0x8000;
					icpi->ssp2.locks = LOCK_B;
					/* 0.1 seconds before decrement */
					icpi->ssp2.tmr_preset = 0x00;
					/* 10 decrements before pulse */
					icpi->ssp2.tmr_count = 0x00;
					/* allow software attention */
					icpo->ssp2.attn_ena = 0x8000;
					icpo->ssp2.locks = LOCK_B;
				}
				/* setup global interval timer for 
				   a 1 second pulse */
				/* enable it */
				icp_glo->ssp2.bus_cntrl |= 0x0D; 

  				mpc = &meg_chan[icp->icp_minor_start];
        		/* fake ldv record */
        		icp->lmx[0].lmx_active = DEV_GOOD;
        		icp->lmx[0].lmx_mpc = mpc;
        		icp->lmx[0].lmx_id = mpd->mpd_id;
        		icp->lmx[1].lmx_active = DEV_BAD;
        		icp->lmx[2].lmx_active = DEV_BAD;
        		icp->lmx[3].lmx_active = DEV_BAD;

				if (mpd->mpd_bus == PCI_BUS) {
					if ((mpd->mpd_id & 0xF8) == 0xA8) {
              					icp->lmx[0].lmx_chan = 2;
              					icp->lmx[0].lmx_speed = 3;
					} else {
              					icp->lmx[0].lmx_chan = 4;
              					icp->lmx[0].lmx_speed = 3;
					}
				} else {
          				if((mpd->mpd_id & 0xF8) == SSP4_ID) {
              					icp->lmx[0].lmx_chan = 4;
              					icp->lmx[0].lmx_speed = 3; /* Max 920 */
          				} else  {
              					icp->lmx[0].lmx_chan = 2;
              					icp->lmx[0].lmx_speed = 3;
          				}
				}
	
          	icp->lmx[0].lmx_type = 0;
          	icp->lmx[0].lmx_rmt_active = 0;
          	icp->lmx[0].lmx_good_count = 0;
          	icp->lmx[0].lmx_wait = -1;
				/* fake ring state flags */
    				icp->icp_rng_state = RNG_GOOD;
				/* set "ring bad" last pass */
				icp->icp_rng_last = 0x04;
				icp_glo->ssp2.on_line = 0x01;
			}
			/* deselect page and leave board off */
			if(mpd->mpd_nwin == HA_WIN16) {
                          if(mpd->mpd_board_def->asic == SSP64)
			    outw(0x00, mpd->mpd_pgctrl );
			  else
			    outb(0x00,mpd->mpd_pgctrl);
			}
 		}		/* loop nicps */
		/* deselect page and leave board off */
		if(mpd->mpd_nwin == HA_WIN16) {
                  if(mpd->mpd_board_def->asic == SSP64)
		    outw(0x00, mpd->mpd_pgctrl );
		  else
		    outb(0x00,mpd->mpd_pgctrl);
		}

		/* PatH Store Multimodem country code and print to log */
		if (mpd->mpd_board_def->flags & MM)
		{
			c_code = *(((char *) mpd->mpd_mem ) + MM_COUNTRY_CODE_REG) 
							& 0x7F;
			mpd->mpd_ccode = c_code;
			printk ("EQUINOX: Multimodem board country code ID is %X\n",
						   	c_code);
		}
	}		/* loop Adapters */

	mpd = meg_dev;

	printk("EQNX: Driver Enabled for %d board(s).\n",nmegaport);



#ifndef MODULE
	eqnx_meminit(kmem_start);
#endif

/*
 *	Allocate a temporary write buffer.
 */
	eqnx_tmpwritebuf = (char *) eqnx_memalloc(XMIT_BUF_SIZE);
	if (eqnx_tmpwritebuf == (char *) NULL)
		printk("EQUINOX: failed to allocate memory (size=%d)\n", 
			XMIT_BUF_SIZE);
	eqnx_txcookbuf = (char *) eqnx_memalloc(XMIT_BUF_SIZE);
	if (eqnx_txcookbuf == (char *) NULL)
		printk("EQUINOX: failed to allocate memory (size=%d)\n", 
			XMIT_BUF_SIZE);

/*	Set up a character driver for ssdiag and sstty.
 */

	if ((diag_num = register_chrdev( 0, "eqnxdiag", &eqnx_fdiag)) <= 0)
		printk("EQUINOX: failed to register eqnx diag device\n");

	/* Initialize the tty_driver structure */
    
	if (register_eqnx_dev() != 0)
		return(-1);

	init_timer(&eqnx_timer);
	eqnx_timer.expires = jiffies + MPTIMEO;
	eqnx_timer.data = 0;
	eqnx_timer.function = &sstpoll;
	add_timer(&eqnx_timer);

#ifndef MODULE
	kmem_start = eqnx_memhalt();
#endif
#ifdef DEBUG
	printk("EQNX: registered EQNX driver\n");
#endif
	return kmem_start;
}

/*****************************************************************************/

/*
 *	Local memory allocation routines. These are used so we can deal with
 *	memory allocation at init time and during run-time in a consistent
 *	way. Everbody just calls the eqnx_memalloc routine to allocate
 *	memory and it will do the right thing. There is no common memory
 *	deallocation code - since this is only done is special cases, all of
 *	which are tightly controlled.
 */

#ifndef MODULE

static void eqnx_meminit(long base)
{
	eqnx_memend = base;
	eqnx_meminited = 1;
}

static long eqnx_memhalt(void)
{
	eqnx_meminited = 0;
	return(eqnx_memend);
}

#endif

static void *eqnx_memalloc(int len)
{
	void	*mem;

	if (eqnx_meminited) {
		mem = (void *) eqnx_memend;
		eqnx_memend += len;
	} else {
		mem = (void *) kmalloc(len, GFP_KERNEL);
	}
	return(mem);
}

/*****************************************************************************/

#ifdef MODULE

/*
 *	Include kernel version number for modules.
 */
int init_module()
{
	printk("init_module()\n");
	return((int) eqnx_init(0));
}

/*****************************************************************************/

/*
** cleanup_module()
**
** cleanup on module unload
*/
void cleanup_module(void)
{
	struct mpdev *mpd;
	struct mpchan *mpc;
	icpgaddr_t icpg;
	icpaddr_t icp;
	unsigned long flags;
	int i, j, jj, m = 0, n = 0, k, win16, ssp_channels;

	printk("Unloading %s Version %s\n", Version, VERSNUM);
	printk("%s\n", Copyright);

#ifdef DEBUG
	printk("cleanup_module: trying to unregister eqnx_driver\n");
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	printk("cleanup_module:eqnx_refcount = %d\n", eqnx_refcount);
#endif
#endif
	for (i =0, j = 0; i < nmegaport; i += 2, j++){
		n = tty_unregister_driver(&eqnx_driver[j]);
#if (LINUX_VERSION_CODE < 132096)
		m = tty_unregister_driver(&eqnx_callout_driver[j]);
#endif /* 2.2 kernels */
	}
	if ( m || n){
	   printk("eqnx: Failed to unregister the tty driver, errno = %d %d\n",
		-m, -n);
		return;
	}
	if (timer_pending(&eqnx_timer))
		del_timer(&eqnx_timer);

  	if ( to_eqn_ramp_admin != -1 ) {
		if (timer_pending(&eqnx_ramp_timer))
			del_timer(&eqnx_ramp_timer);
		to_eqn_ramp_admin = -1;
	}

	/* free the normaltermios and callouttermios pointers */
	for( k = 0; k < nmegaport; k++) {    /* for each board */
	 /* map Adapter memory */
	 mpd = &meg_dev[k];
	 
	 for( j = 0; j < (int)mpd->mpd_nicps; j++) {  /* for each icp */
	    if (mpd->mpd_board_def->asic == SSP64) /* SSP64 */
		    ssp_channels = 64;
            else /* SSP4 board */ 
            {
               if (mpd->mpd_board_def->number_of_ports > 2)
		       ssp_channels = 4;
	       else    ssp_channels = 2;
	    }

            /* go free the per channel termios structures */
            for ( i = 0; i <  ssp_channels; i++ ) {
               /* setup mpc virtual addresses for cpu access 
                  of icp */
               jj = mpd->mpd_minor + i + 
               (j * mpd->mpd_sspchan);
               mpc = &meg_chan[jj];

               if( mpc->normaltermios != NULL ) 
                  vfree( (void *)mpc->normaltermios ); 
#if (LINUX_VERSION_CODE < 132096)
               if( mpc->callouttermios != NULL ) 
                  vfree( (void *)mpc->callouttermios ); 
#endif
            }
         }              /* for nicps */
       }                /* for nmegaports */
       /* end free the normaltermios and callouttermios pointers */

	if (eqnx_ttys != (struct tty_struct **) NULL)
		vfree((void *)eqnx_ttys);
	if (eqnx_termios != (struct termios **) NULL)
		vfree((void *) eqnx_termios);
	if (eqnx_termioslocked != (struct termios **) NULL)
		vfree((void *)eqnx_termioslocked);
	if (eqnx_driver != (struct tty_driver *) NULL)
		vfree((void *)eqnx_driver);
#if (LINUX_VERSION_CODE < 132096)
	if (eqnx_callout_driver != (struct tty_driver *) NULL)
		vfree((void *)eqnx_callout_driver);
#endif
#ifdef DEBUG
	printk("cleanup_module: trying to unregister eqnx_diag\n");
#endif
	if ((i = unregister_chrdev(diag_num, "eqnxdiag")))
		printk("EQUINOX: failed to unregister diag device, errno=%d\n",
			-i);
#ifdef DEBUG
	printk("cleanup_module: trying to free meg_chan\n");
#endif
	if (meg_chan != (struct mpchan *) NULL)
		vfree((void * )meg_chan);
	if (eqnPCIcsh != (char *) NULL)
		vfree((void * )eqnPCIcsh);
	if (eqnx_tmpwritebuf != (char *) NULL)
		kfree(eqnx_tmpwritebuf);
	if (eqnx_txcookbuf != (char *) NULL)
		kfree(eqnx_txcookbuf);
	for(k=0; k < nmegaport; k++){
		mpd = &meg_dev[k];
		if (mpd->mpd_alive == 0)
			continue;
		else
			mpd->mpd_alive = 0;

	 	spin_lock_irqsave(&mpd->mpd_lock, flags);
		if (mpd->mpd_board_def->asic == SSP64) {
			/* SSP64 */
			win16 = mpd->mpd_nwin;
			for(i = 0; i < (int)mpd->mpd_nicps;i++){
				if(win16)
					mega_push_winicp(mpd, i);
				icp = &mpd->icp[i];
				icpg = (icpgaddr_t) ((unsigned long)icp->icp_regs_start
					+0x2000);
         			/* turn off ring clock, PRAM and DMA */
         			printk("turn off ring clock, PRAM and DMA\n");
				Gicp_initiate &= ~0x1e;
         			/* reset icp to known state */
				i_gicp_attn = 0;
				Gicp_initiate = 0;
				icpg->ssp.gicp_watchdog = 0;
				if(win16)
					mega_pop_win(9);
			}
		}
	 	spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		if (mpd->mpd_bus == ISA_BUS) {
			release_region(portn[mpd->mpd_io],8);
		}
		else {
#if (EQNX_VERSION_CODE < 131328)
			if (mpd->mpd_mem > 0x100000)
#endif
			    iounmap((void *) mpd->mpd_mem);
		}

#if	(LINUX_VERSION_CODE >= 132096)
#ifdef	ISA_ENAB
#ifdef	CONFIG_ISAPNP
	/*
	** ISA plug-and-play in linux kernels 2.4+
	*/
	for (i=0; i<pnp_found; i++) {
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.4 kernels */
		pnp_found_devs[i]->deactivate(pnp_found_devs[i]);
#endif
		pnp_found_devs[i] = NULL;
	}

	pnp_found = 0;
#endif	/* CONFIG_ISAPNP */
#endif
#endif

	}
}
#endif

/*
** test_8bit_hole(mpd, k, addr)
*/
int test_8bit_hole(struct mpdev *mpd, int k, paddr_t addr )
{
int ret=true;
int ram_index;
ushort_t *ramw;
int ii, jj,j;
paddr_t mpd_mem;
uchar_t cchnl;
ushort_t no_cache;
icpgaddr_t icpg;
icpaddr_t icp;
volatile unsigned char *chnl_ptr;
int testlen;
int ssp_channels = 0;
volatile uchar_t *bus_ctrl_p;

	printk("EQNX: test_8bit port (%x) addr (%x)\n",
                (unsigned int) addr,(unsigned int) portn[k]);

  if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    /* Set the address, 16-bit mode, MAX2, and flat off */ 
    outb((addr >> 14) & 0xff,portn[k]);
    outb(((addr >> 22) & 3)| 0x20, portn[k] + 1);
    /* page register - deselect ram */  
    outb(0, portn[k] + 2);
    outb(0, portn[k] + 3);
    /* disable interrupts */ 
    outb(0, portn[k] + 6);
  }
  else { /* SSP4 board */
      unsigned char b;
     
      /* reg 8 (page reg) */
      if(mpd->mpd_nwin == HA_FLAT)
         b = 0x20;
      else
         b = 0;

      outb( b, portn[mpd->mpd_io] + 0x08);

      b = ((addr & 0x0000c000) >> 8);
      /* no ints */
      outb(b, portn[mpd->mpd_io] + 0x09);
      b = ((addr & 0xFFFF0000) >> 16);
      outb(b, portn[mpd->mpd_io] + 0x0a);
      /* reg b*/
      b=0;
      outb(b, portn[mpd->mpd_io] + 0x0b);
  }
	mpd_mem = addr;
        if(!mpd_mem)
        {
	   printk("EQNX: Memory mapping failed for board %d\n", k +1);
           ret= false;
           return(ret);
        }

        /*  1 icps */
         j=0;

	/* select correct page if necessary */
        if ( mpd->mpd_nwin == HA_WIN16 ){
          if (mpd->mpd_board_def->asic == SSP64)
	    /* SSP64 */
            outw( 0x0100 | j, portn[k] + 2 );
          else
	    /* SSP2 / SSP4 */
            outb( 1 << j,portn[k] + 8); 
        }

	/* reset icp to known state */
        icp = &mpd->icp[j];
        if (mpd->mpd_board_def->asic == SSP64) {  /* SSP64 */
          icpg = (icpgaddr_t)((unsigned long)mpd_mem+0x2000);
          i_gicp_attn = 0;
          Gicp_initiate = 0;
	  /* Adjust the number of icps on the board  */
          icpg->ssp.gicp_bus_ctrl = 0x09;
	  bus_ctrl_p = &(icpg->ssp.gicp_bus_ctrl);
        }
        else { /* SSP2/4 */
          /* VIRTUAL- ptr to global regs */
          volatile union global_regs_u *icp_glo =
            (volatile union global_regs_u *)((unsigned long)mpd_mem + 0x400);
          /* VIRTUAL ptr to output regs */
          volatile union output_regs_u *icp_cout =
            (volatile union output_regs_u *)((unsigned long)mpd_mem + 0x200);
          /* VIRTUAL ptr to input regs */
          volatile union input_regs_u *icp_cin =
            (volatile union input_regs_u *)mpd_mem;

	  icp_glo->ssp2.on_line = 0;   /* disable SSP2 */
	  icp_cin->ssp2.locks = 0xFF;/* input locks SSP2 */
          /* output locks SSP2 */
	  icp_cout->ssp2.locks = 0x77;
          /* Set Bus Control = 16 bits */
          icp_glo->ssp2.bus_cntrl = 0x01;
	  bus_ctrl_p = &(icp_glo->ssp2.bus_cntrl);
	  if (mpd->mpd_board_def->bus == PCI_BUS)
	  	ssp_channels = mpd->mpd_board_def->number_of_ports;
	  else {
		if (mpd->mpd_board_def->number_of_ports > 2)
			ssp_channels = 4;
		else	ssp_channels = 2;
	  }

          for(ii = 0; ii < ssp_channels; ii++) /* set all locks */
          {
              (icp_cin + ii)->ssp2.locks = 0xFF;
              (icp_cout + ii)->ssp2.locks = 0x77;
              (icp_cout + ii)->ssp2.cntrl_sigs = 0x0F;
          }
        }
	

  /* test pram  - 16-bit test */
  ramw = (ushort_t *) &(mpd->icp[j].icp_regs_start);
  if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    testlen = HWREGSLEN;
    ram_index = mem_test_pram(mpd, ramw, testlen, j);
  }
  else { /* SSP2/4 board */
    if ( mpd->mpd_nwin == HA_FLAT )
       outb( mpd->mpd_pgctrl, 0x20 );  /*SSP2 enables all for HA_FLAT */

    if (mpd->mpd_bus == PCI_BUS)
      testlen = ((mpd->mpd_id & 0xFF) == 0xA8) ? 0x100 : 0x200;
    else
      testlen = ((mpd->mpd_id & 0xFF) == SSP2_ID) ? 0x100 : 0x200;
    ram_index = mem_test_pram(mpd, ramw, testlen, j);
    if (!ram_index) {
      ramw = (ushort_t *)((unsigned long)(mpd->icp[j].icp_regs_start) + 0x200);
      ram_index = mem_test_pram(mpd, ramw, testlen, j);
    }
  }
  if ( ram_index )
  {
      printk("EQNX: PRAM memory test failure %d for board with I/O address 0x%x.\n",ram_index,portn[k]);
     ret= false;   /*need to de-map */
  }
  

  /* zero pram */
  ramw = (ushort_t *) mpd_mem;

  if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    for ( ii = 0; ii < HWREGSLEN; ii += 2, ramw++ )
    {
      jj = ii & 0x7f;
      if ( ii >= HWREGSLEN/2  /* upper 8k of range includes global regs */
           && ((jj >= 0x18 && jj < 0x20)
                || (jj >= 0x38 && jj < 0x40)
                || (jj >= 0x58 && jj < 0x60)
                || (jj >= 0x78 && jj < 0x80)) )
        continue;
      *ramw = 0;
    }
  }
  else { /* SSP2/4 board */
    for ( ii = 0; ii < 0x400; ii += 2, ramw++ )
    {
      jj = ii & 0x7f;
      if (jj == 0x02 || 
          jj == 0x64 ||
          jj == 0x72  )  /* protect sensitive regs  */
        continue; 
      *ramw = 0;
    }
  }
  /* verify that pram is not cached */

    if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
        icpg = (icpgaddr_t)((unsigned long)mpd_mem + 0x2000);
       /* verify that pram is not cached */

      cchnl = Gicp_chnl;
      chnl_ptr = &(Gicp_chnl);
    }
    else { /* SSP4 board */
      icpaddr_t icp;
      volatile union global_regs_u *icp_glo ;
      icp = &mpd->icp[j];
      icp_glo = (volatile union global_regs_u *)((unsigned long)icp->icp_regs_start + 0x400);
      cchnl = icp_glo->ssp2.chan_ctr;
      chnl_ptr = &(icp_glo->ssp2.chan_ctr);
    }
      no_cache = false;
      for ( ii = 0; ii < 0x10000; ii++ )
        if ( *chnl_ptr != cchnl )
        {
          no_cache = true;
          break;
        }
  if ( !no_cache )
  {
          printk("EQNX: PRAM memory appears to be cached %d for with I/O address 0x%x.\n",
          ii,portn[k]);
         ret=false;
  }
    *bus_ctrl_p = 0x0;           /* turn off */

    if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    outw( 0x0000, portn[k] + 2) ;              /*Disable Controller's addressing                       */
    outw( 0x0000, portn[k]) ;
    }
    else { /* SSP4 board */
      unsigned char b = inb(portn[k] + 8);
      outb(b & 0xF0, portn[k] + 8);
    }

  return(ret);
}

/****************************************************************************

	EISA_MEM_CFG

	Place pointers to buffer, tag and cmd memory in the icp records
	pointed to be "mpd".  Also place the corresponding lengths in 
	appropriate fields.

	As additional board types are added, this function must be updated.

*****************************************************************************/

void brd_mem_cfg( struct mpdev *mpd )
{
int i;

  for ( i = 0; i < (int)mpd->mpd_nicps; i++ )
  {
    if(mpd->mpd_nwin == HA_FLAT) {
      if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
	/* eisa memory config icp */
	mpd->icp[i].icp_regs_start = (mpaddr_t)((unsigned long)mpd->mpd_mem + (i * 0x4000));
	/* Special case PCI 64 port boards */
	if ((mpd->mpd_bus == PCI_BUS) && 
		((mpd->mpd_id & 0xF8) == 0x08))
	mpd->icp[i].icp_dram_start = ((unsigned long)mpd->mpd_mem + 
		mpd->mpd_memsz/2);
	else
	mpd->icp[i].icp_dram_start = ((unsigned long)mpd->mpd_mem + 
				mpd->mpd_memsz + (i * mpd->mpd_memsz / 2));
        if((mpd->mpd_id & 0xF8) == 0x18 || (mpd->mpd_id & 0xF8) == 0x20 ||
           (mpd->mpd_id & 0xF8) == 0x30 || (mpd->mpd_id & 0xF8) == 0x38 ) {

            if ((mpd->mpd_id & 0xF8) == 0x30 )
              mpd->mpd_hwq = &sst_hwq[2]; /* 1k */
            else
               if ((mpd->mpd_id & 0xF8) == 0x38 )
                 mpd->mpd_hwq = &sst_hwq[3]; /* .5 k */
               else
                  mpd->mpd_hwq = &sst_hwq[1]; /* 2k */
           mpd->icp[i].icp_tags_start = ((unsigned long)mpd->mpd_mem + 0x4000);
	   mpd->icp[i].icp_cmds_start = ((unsigned long)mpd->mpd_mem + 0x4000);
	} else {
	   mpd->icp[i].icp_tags_start = ((unsigned long)mpd->mpd_mem + 0x40000 + (i * 0x20000));
	   mpd->icp[i].icp_cmds_start = ((unsigned long)mpd->mpd_mem + 0x40000 + (i * 0x20000));
	   mpd->mpd_hwq = &sst_hwq[0]; /* 4k for SS64 & SS128 */
	} /* mpd_id */
      } /* SSP64 */
      else { /* SSP4 */
	mpd->icp[i].icp_regs_start = (mpaddr_t)((unsigned long)mpd->mpd_mem + 
          (i * sizeof(struct ssp2_addr_space_s)));
	mpd->icp[i].icp_dram_start = ((unsigned long)mpd->mpd_mem + 
	  (i * sizeof(struct ssp2_addr_space_s)) + 0x1000);
        mpd->icp[i].icp_tags_start = ((unsigned long)mpd->mpd_mem + 0x3000 + 
          (i * sizeof(struct ssp2_addr_space_s)));
        /* Note: cmds doesn't exist on SSP2, so point to tags */
        mpd->icp[i].icp_cmds_start = mpd->icp[i].icp_tags_start;
        mpd->mpd_hwq = &sst_hwq[2];
      } /* SSP4 */
    } /* HA_FLAT */
    else {  /* HA_WIN */
      if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
	mpd->icp[i].icp_regs_start = (mpaddr_t)((unsigned long)mpd->mpd_mem);
	mpd->icp[i].icp_dram_start = (unsigned long)mpd->mpd_mem;
	mpd->icp[i].icp_tags_start = (unsigned long)mpd->mpd_mem;
	mpd->icp[i].icp_cmds_start = (unsigned long)mpd->mpd_mem;
         if ((mpd->mpd_id & 0xF8) == 0x30 )
           mpd->mpd_hwq = &sst_hwq[2]; /* 1k */
         else
            if ((mpd->mpd_id & 0xF8) == 0x38)
              mpd->mpd_hwq = &sst_hwq[3]; /* .5 k */
          else
            if((mpd->mpd_id & 0xF8) == 0x18 || (mpd->mpd_id & 0xF8) == 0x20)
                mpd->mpd_hwq = &sst_hwq[1]; /* 2k */
	else
		mpd->mpd_hwq = &sst_hwq[0]; /* 4k for SS64 & SS128 */
      } /* SSP64 board */
      else { /* SSP4 board */
	mpd->icp[i].icp_regs_start = (mpaddr_t)((unsigned long)mpd->mpd_mem);
	mpd->icp[i].icp_dram_start = (unsigned long)mpd->mpd_mem + 0x1000;
	mpd->icp[i].icp_tags_start = (unsigned long)mpd->mpd_mem + 0x3000;
	mpd->icp[i].icp_cmds_start = (unsigned long)mpd->mpd_mem + 0x3000;
	mpd->mpd_hwq = &sst_hwq[2]; /* 1k */
      }
    } /* HA_WIN */	
  } /* for */
}  /* eisa_mem_cfg */

/*
** mem_test_pram(mpd, ramw, testlen, icp)
*/
int mem_test_pram( struct mpdev *mpd, ushort_t *ramw, int testlen, int icp )
{
  int ram_ok = 0;
  int ii, jj;

  for ( ii = 0; ii < testlen; ii += 2, ramw++ )  /* include input & output */
  {
    /* select correct page if necessary */
    if(mpd->mpd_nwin == HA_WIN16) {
      if (mpd->mpd_board_def->asic == SSP64) 
	 /* SSP64 */
         outw(0x100 | icp, mpd->mpd_pgctrl);
      else
	 /* SSP2 / SSP4 */
         outb( 1 << icp, mpd->mpd_pgctrl);
    }
    if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 only */
      jj = ii & 0x7f;
      if ( ii >= HWREGSLEN/2  /* upper 8k of range includes global regs */
           && ((jj >= 0x18 && jj < 0x20)
                || (jj >= 0x38 && jj < 0x40)
                || (jj >= 0x58 && jj < 0x60)
                || (jj >= 0x78 && jj < 0x80) ))
        continue;
    }
    *ramw = 0x55aa;
    ram_ok = true;
    if ( *ramw != 0x55aa )
    {
      ram_ok = false;
      break;
    }
    *ramw = 0xaa55;
    if ( *ramw != 0xaa55 )
    {
      ram_ok = false;
      break;
    }
  }
  if (ram_ok)
    ii = 0;
  return(ii);
} 

/*
** mem_test_dram(mpd, ramw, testlen, icp)
*/
int mem_test_dram( struct mpdev *mpd, ushort_t *ramw, int testlen, int icp )
{
  int ram_ok;
  uchar_t ram_pg;
  int ii;

  ram_ok = true;
  ram_pg = 0;
  for ( ii = 0; ii < testlen; ii += 2, ramw++ )
  {
    /* select correct page if necessary */
    if(mpd->mpd_nwin == HA_WIN16 && !(ii % 0x4000)) {
      if (mpd->mpd_board_def->asic == SSP64)
	 /* SSP64 */
         outw(0x300 | ram_pg++, mpd->mpd_pgctrl);
      else
	 /* SSP2 / SSP4 */
         outb( 1 << icp, mpd->mpd_pgctrl);

       if(ii)
    	 ramw = (ushort_t *) ((uchar_t *)ramw - 0x4000);
      }
    *ramw = 0x55aa;
    if ( *ramw != 0x55aa )
    {
      ram_ok = false;
      break;
    }
    *ramw = 0xaa55;
    if ( *ramw != 0xaa55 )
    {
      ram_ok = false;
      break;
    }
  }
  if (ram_ok)
    ii = 0;
  return(ii);
}

/*
** mem_test_dram(mpd, ramb, testlen, icp)
*/
int mem_test_tag(struct mpdev *mpd, uchar_t *ramb, int testlen, int icp)
{
  int ram_ok;
  uchar_t ram_pg;
  volatile uchar_t *ramb2;
  int ii;

  ramb2 = ramb + 1;
  ram_ok = true;
  ram_pg = 0;
  for ( ii = 0; ii < testlen; ii += 2, ramb += 2, ramb2 += 2 )
  {
    /* select correct page if necessary */
    if(mpd->mpd_nwin == HA_WIN16 && !(ii % 0x4000)) {
      if (mpd->mpd_board_def->asic == SSP64)
	 /* SSP64 */
         outw(0x300 | ram_pg++, mpd->mpd_pgctrl);
      else
	 /* SSP2 / SSP4 */
         outb( 1 << icp,mpd->mpd_pgctrl ); 

        if(ii) {
	  ramb -= 0x4000;
	  ramb2 -= 0x4000;
	}
      }
    *ramb = 0x55;
    *ramb2 = 0xaa;
    if ( *ramb != 0x55
	   || *ramb2 != 0xaa 
	   || !ram_ok )
    {
      ram_ok = false;
      break;
    }
  }
  if (ram_ok)
    ii = 0;
  return(ii);
}

/*
** mem_zero(mpd, ramw, testlen, icp)
*/
static int mem_zero( struct mpdev *mpd, unsigned short *ramw, 
		int testlen, int icp)
{
  int ii, jj;

  for ( ii = 0; ii < testlen; ii += 2, ramw++ )
  {
    /* select correct page if necessary */
    if ( mpd->mpd_nwin == HA_WIN16 ){
      if (mpd->mpd_board_def->asic == SSP64)
	/* SSP64 */
        outw( 0x0100 | icp,mpd->mpd_pgctrl );
      else
	/* SSP2 / SSP4 */
        outb( 1 << icp, mpd->mpd_pgctrl ); 
    }
    if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
      jj = ii & 0x7f;
      if ( ii >= HWREGSLEN/2  /* upper 8k of range includes global regs */
           && ((jj >= 0x18 && jj < 0x20)
                || (jj >= 0x38 && jj < 0x40)
                || (jj >= 0x58 && jj < 0x60)
                || (jj >= 0x78 && jj < 0x80) ))
        continue;
    } else {
           jj = ii & 0x7f;
           if ( (jj == 0x02) || 
                (jj == 0x64) || 
                (jj == 0x72 ))  /* protect sensitive regs  */
             continue; 
    }
    *ramw = 0;
  }
  return(0);
}

/*
** mem_test(mpd, icp)
*/
static int mem_test(struct mpdev *mpd, int icp)
{
int ram_index;
uchar_t *ramb;
ushort_t *ramw;
int err = 0;
int testlen;

  /* test pram  - 16-bit test */
  ramw = (ushort_t *) (mpd->icp[icp].icp_regs_start);
  if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    testlen = HWREGSLEN;
    ram_index = mem_test_pram(mpd, ramw, testlen, icp);
  }
  else { /* SSP2/4 board */
    if ( mpd->mpd_nwin == HA_FLAT )
       outb( 0x20, mpd->mpd_pgctrl );  /*SSP2 enables all for HA_FLAT */

    if (mpd->mpd_bus == PCI_BUS)
      testlen = ((mpd->mpd_id & 0xFF) == 0xA8) ? 0x100 : 0x200;
    else
      testlen = ((mpd->mpd_id & 0xF8) == SSP2_ID) ? 0x100 : 0x200;
    ram_index = mem_test_pram(mpd, ramw, testlen, icp);
    if (!ram_index) {
      ramw = (ushort_t *)((unsigned char *)(mpd->icp[icp].icp_regs_start) + 0x200);
      ram_index = mem_test_pram(mpd, ramw, testlen, icp);
    }
  }
  if ( ram_index )
  {
    if (mpd->mpd_bus == ISA_BUS)
       printk("EQNX: PRAM memory test failure %d for board with I/O address 0x%x.\n",(unsigned int) ram_index,(unsigned int) portn[mpd->mpd_io]);
    else
    printk("EQNX: PRAM memory test failure %d for board in Slot %d.\n",ram_index,mpd->mpd_slot);
    err = 1;
  }
  
  /* test dram - word test */
  if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    ramw = (ushort_t *) (mpd->icp[0].icp_dram_start);  /* dram */
    testlen = mpd->mpd_memsz;
    ram_index = mem_test_dram(mpd, ramw, testlen, icp);
  }
  else { /* SSP2/4 board */
    /* SSP2/4 enable each icp to test dram */
         
    if ( mpd->mpd_nwin == HA_FLAT )
       outb( 0x20, mpd->mpd_pgctrl );  /*SSP2 enables all for HA_FLAT */

    if (mpd->mpd_bus == PCI_BUS)
      testlen = ((mpd->mpd_id & 0xFF) == 0xA8) ? 0x800 : 0x1000;/*2 or4*/
    else
      testlen = ((mpd->mpd_id & 0xf8) == SSP2_ID) ? 0x800 : 0x1000;/*2 or4*/

    /* test input buff */
    ramw = (ushort_t *) (mpd->icp[icp].icp_dram_start);
    ram_index = mem_test_dram( mpd, ramw, testlen, icp); 

    if(!ram_index){
      /* test output buff */
      ramw = (ushort_t *) ((unsigned long)mpd->icp[icp].icp_dram_start + 0x1000);
      ram_index = mem_test_dram( mpd, ramw, testlen, icp); 
    }
  }
  if ( ram_index )
  {
    if (mpd->mpd_bus == ISA_BUS)
       printk("EQNX: DRAM word memory test failure %d for board with I/O address 0x%x.\n",ram_index,portn[mpd->mpd_io]);
    else
       printk("EQNX: DRAM word memory test failure %d for board in Slot %d.\n",ram_index,mpd->mpd_slot);
    err |= 2;
  }

  /* test tag dram - requires BYTE accesses! */
  /* Same values OK for SSP64 and SSP4 */
  ramb = (uchar_t *) (mpd->icp[0].icp_tags_start);  /* tags ram */
  if (mpd->mpd_board_def->asic == SSP64)  /* SSP64 board */
    testlen = mpd->mpd_memsz/4;
  else {
    if (mpd->mpd_bus == PCI_BUS)
      testlen = ((mpd->mpd_id & 0xFF) == 0xA8) ? 0x200 : 0x400;/*2 or4*/
    else
      testlen = ((mpd->mpd_id & 0xf8) == SSP2_ID) ? 0x200 : 0x400;/*2 or4*/
  }
  ram_index = mem_test_tag(mpd, ramb, testlen, icp);
  if ( ram_index )
  {
    if (mpd->mpd_bus == ISA_BUS)
       printk("EQNX: DRAM tags memory test failure %d for board with I/O address 0x%x.\n",ram_index,portn[mpd->mpd_io]);
    else
       printk("EQNX: DRAM tags memory test failure %d for board in Slot %d.\n",ram_index,mpd->mpd_slot);
    err |= 4;
  }
  /* zero pram */
  ramw = (ushort_t *) (mpd->icp[icp].icp_regs_start);
/*
printk("clear pram icp=%d  ramw=%x\n",icp,(unsigned int) ramw);
*/
  if (mpd->mpd_board_def->asic == SSP64)  /* SSP64 board */
    testlen = HWREGSLEN;
  else  /* SSP4 board */
    testlen = 0x400;
  mem_zero(mpd, ramw, testlen, icp);
  return(err);
}

/******************************************************************************

	EISA_BRDTYP_OK

	Return true for supported boards, else false.

******************************************************************************/

int eisa_brdtyp_ok( ushort_t bbbbbrrr, ushort_t cfg )
{
  bbbbbrrr &= 0xf8;
  if(bbbbbrrr < SSP2_ID)
  {
     if ( bbbbbrrr == 0x08
	  || bbbbbrrr == 0x10
	  || bbbbbrrr == 0x18
	  || bbbbbrrr == 0x20
	  || bbbbbrrr == 0x30
	  || bbbbbrrr == 0x38 )
       return true;
   }
   else
   {
      if( bbbbbrrr == SSP2_ID || bbbbbrrr == SSP4_ID ) 
      {
         if(cfg == 0x1b   /* SSP2/4 valid configurations */
            || cfg == 0x09
            || cfg == 0x24 )
          return true;
       }
   }
   return false;
}  /* eisa_brdtyp_ok */

#ifdef	ISA_ENAB
/******************************************************************************

	EISA_BRD_FOUND

	Examine EISA "slot" for an Equinox board.  If not found return false.
	If found, fill "regs" from the eisa registers.  Test that the board is
	enabled.  If so, return true, else -1.

	As additional board types are added, this function MAY have to be 
	updated.

******************************************************************************/

static int eisa_brd_found( int slot, ushort_t *regs )
{
ushort_t port, bid, port_org;
volatile ushort_t *regs_org;
volatile ushort_t *regs_enb;

  regs_org = regs;
  regs_enb = regs + 2;
  port = slot * 0x1000 + 0xC80;
  port_org = port;
  *regs = inw( port );        /* 0x?000 */
  if ( *regs != EISA_EQX )
    return false;

  regs++;
  port += 2;
  *regs++ = inw( port );    /* 0x?002 */
  port += 2;
  *regs = inw( port );    /* 0x?004 */

  port += 2;
  regs++;
  bid = regs_org[2] & (SSP4_MASK << 8);
  

  if((bid & 0xF800) < (SSP2_ASIC << 8)){	/* SSP64 */
     while ( (*regs = inb( port )) & 0xc0 );  /* 0x?006 - align w/zero */
     regs++;
     *regs++ = inb( port );    /* 0x?008  (regs[4])*/
     *regs++ = inb( port );    /* 0x?00A */
     *regs++ = inb( port );    /* 0x?00C */

  /* create a "standard" board id/rev byte and store at array index 7 64/128 */
     *regs++ = ((regs_org[2] >> 8) & 0xf8) | ((regs_org[3] >> 11) & 0x07);

     *regs = cmn_irq[ regs_org[3] & 0x07 ];  /* array index 8 */

     if ( !(*regs_enb & 0x01) )  /* enabled? */
        return -1;

     if ( *regs == 0xff )
     {
        if ( eisa_brdtyp_ok( regs_org[7] , regs_org[9] )) {
           return true;
	}
	else
           return -3;
      }

  } else { 	/* SSP2/4 */	
     ushort u1, v1, pp;

     port += 2;                /* Skip 2 unused ports */
     *regs++ = inw( port );    /* Device Select,MA15/MA14 (regs[3]) */	
     outw(regs_org[3] | 0x1000,port);
     port += 2;
     *regs++ = inw( port );    /* A23-A16, MA31-A24 (regs[4]) */ 
     *regs++ = 0;
     *regs++ = 0;
     *regs++ = 0;
     *regs++ = 0;


     /* create a standard board byte at array index 10 */
     /* must toggle rmod to both states */
     pp = slot * 0x1000 + 0x0c80;
     u1 = inw( (pp + 8));  /* port 8/9 */
     outw( (u1 | 0x1000), (pp + 8));  /* Set special duty don't hang bit */
     u1 = inw( (pp + 8));  /* port 8/9 */
     outw( (u1 | 0x0080), (pp + 8)); 
     v1 = inw( (pp + 6));  /* port 6/7 */
     regs_org[10] = ((ushort_t) (v1 & 0x3800) >> 11);	/*  set lower 3 bits */
     outw( ( u1 & ~0x0080 ), (pp + 8));	/* toggle rmod */
     v1 = inw( (pp + 6));  /* port 6/7 */
     regs_org[10] |= ((ushort_t) (v1 & 0x3800) >> 8); /* or in upper bits */

  /* board id/rev byte and store at array index 9 SSP2/4 */
     *regs++ = ((regs_org[2] >> 8) & 0xff) | ((regs_org[1] >> 8) & 0x07);

     /* 
      * Set array index 7 to qualifier(high byte) and board ID(low byte).
      */
     regs_org[7] = (regs_org[10] << 8) | regs_org[9];

     if ( !(*regs_enb & 0x01) ) {  /* enabled? */
        return -1;
     }

     if ( eisa_brdtyp_ok( (regs_org[2] >>8) & 0xF8  , regs_org[10])) {
        return true;
     }
     else {
        return -3;
     }

   }
    return -2;
}  /* eisa_brd_found */
#endif	/* ISA_ENAB */

#ifdef	ISA_ENAB
/******************************************************************************

	EISA_BASE_PADDR

	Return the mapped physical address for the board.  "regs" contains
	the array of Equinox registers.

	As additional board types are added, this function must be updated.

******************************************************************************/

static paddr_t eisa_base_paddr( ushort_t *regs )
{
ushort_t bid;
paddr_t p = 0;
uint_t w;


  bid = regs[2] & (SSP4_MASK << 8);

  if((bid & 0xF800) < (SSP2_ASIC << 8) ){	/* SSP64 */
     w = *(regs + 4);
     w = (w & 0x3f) << 14;
     p |= (paddr_t) w;
     w = *(regs + 5);
     w = (w & 0x3f) << 20;
     p |= (paddr_t) w;
     w = *(regs + 6);
     w = (w & 0x3f) << 26;
     p |= (paddr_t) w;
     return p;
  } else {	/* SSP2/4 */
     w = *(regs + 3);
     w = (w & 0xC0) ;
     p |= (paddr_t) w;
     w = *(regs + 4);
     w = (w & 0xff) << 16;
     p |= (paddr_t) w;
     w = *(regs + 4);
     w = (w & 0xff00) << 16;
     p |= (paddr_t) w;
  }
  return p;
}  /* eisa_base_paddr */

/******************************************************************************

	EISA_MEM_SIZE

	Return the size of the memory map required by the board
	described by "regs".
	
	req == 0 return the size of memory to map
	req == 1 return the amount of buffer dram

	As additional board types are added, this function must be updated.

	This name is a public symbol in Unisys v1.2.

******************************************************************************/

static uint_t eisa_mem_size( ushort_t *regs, int req )
{
ushort_t w;
ushort_t id, bid;

  if ( !req )
  {
    /* return size of footprint */
  bid = regs[2] & (SSP4_MASK << 8);

  if((bid & 0xF800) < (SSP2_ASIC << 8) ){	/* SSP64 */
    w = *(regs + 3);
    id = regs[7] & SSP4_MASK;
  } else {				/* SSP2/4 */
    w = *(regs + 3);
    id = regs[2] & (SSP4_MASK << 8);
    id = (id >> 8);
  }

    switch ( id )
    {
      case 0x10:
        if ( w & 0x0010 )
          return FLAT128_MEM_LEN;
        else
          return WIN16_MEM_LEN;
      case 0x08:
        if ( w & 0x0010 )
          return FLAT64_MEM_LEN;
        else
          return WIN16_MEM_LEN;
      case 0x18:
      case 0x20:
        if ( w & 0x0010 )
          return FLAT8_MEM_LEN;
        else
          return WIN16_MEM_LEN;
       case SSP2_ID:
       case SSP4_ID:
	if ( w & 0x20 ) {		/* Flat mode SSP2/4; 16K * ASIC */
		switch(regs[10] & 0x3F) {  /* Switch on qualifier ID */
			case 0:
			case 0x24:
			case 0x09:
			case 0x36: /* MPM-4 */
				return(WIN16_MEM_LEN);
				break;
			case 0x10:
			case 0x1B:
			case 0x1A:
				return(WIN16_MEM_LEN * 2);
				break;
			case 0x18:
			case 0x20:
			case 0x08:
				return(WIN16_MEM_LEN * 4);
				break;
			default:
				return(WIN16_MEM_LEN * 4);
				break;
		}
	}
	else
          return WIN16_MEM_LEN;  /* Overlayed ASICS 16K  */
      case 0x30:
      case 0x38:
        if ( w & 0x0010 ) 
	   return 0x10000; 
	else
	   return 0x8000;
      default:
	return 0;
    }
  }
  else  /* req == 1 */
  {
    bid = regs[2] & (SSP4_MASK << 8);
    if((bid & 0xF8) < (SSP2_ASIC << 8) ){	/* SSP64 */
       w = *(regs + 3);
       id = regs[7] & SSP4_MASK;
    } else {				/* SSP2/4 */
       w = *(regs + 3);
       id = regs[2] & (SSP4_MASK << 8);
       id = (id >> 8);
    }
    /* return size of RAM */
    switch ( id  )
    {
      case 0x10:
        return 0x100000;  /* 1 meg */
      case 0x08:
        return 0x80000;   /* 1/2 meg */
      case 0x18:
      case 0x20:
      case 0x30:
      case 0x38:
	return 0x8000;    /* 32k */
      case SSP2_ID:
      case SSP4_ID:
	if ( w & 0x20 )		/* Flat mode SSP2/4; 16K num ICP */
		switch(regs[10] & 0x3F) {  /* Switch on qualifier ID */
			case 0:
			case 0x24:
			case 0x09:
			case 0x36: /* MPM-4 */
				return(WIN16_MEM_LEN);
				break;
			case 0x10:
			case 0x1B:
			case 0x1A:
				return(WIN16_MEM_LEN * 2);
				break;
			case 0x18:
			case 0x20:
			case 0x08:
				return(WIN16_MEM_LEN * 4);
				break;
			default:
				return(WIN16_MEM_LEN * 4);
				break;
		}
	else
          return 0x4000;  /* Overlayed ASICS 16K  */
      default:
	return 0;
    }
  }
}  /* eisa_mem_size */
#endif	/* ISA_ENAB */

static uint_t pci_mem_size(struct brdtab_t *brd_def)
{

	if (brd_def->bus == PCI_BUS) {
		if (brd_def->number_of_ports >= 64)
			return FLAT128_MEM_LEN;
		else	return FLAT64K_MEM_LEN;
	} else	return 0;
}  /* pci_mem_size */

#ifdef	ISA_ENAB
/******************************************************************************

	EISA_MEM_STRAT

	Return the memory access strategy required by the board
	described by "regs".

	As additional board types are added, this function must be updated.

******************************************************************************/

static uint_t eisa_mem_strat( ushort_t *regs )
{
ushort_t bid;
ushort_t w;

  bid = regs[2] & (SSP4_MASK << 8);
  if((bid & 0xF800) < (SSP2_ASIC << 8) ){	/* SSP64 */
    w = *(regs + 3);
    if ( w & 0x0010 )
       return HA_FLAT;
    else
       return HA_WIN16;
  } else {				/* SSP2/4 */
    w = *(regs + 3);
    if ( w & 0x20 )		/* Flat mode SSP2/4; 16K * ASIC */
       return HA_FLAT;
    else
       return HA_WIN16;  /* Overlayed ASICS 16K  */
  }
}  /* eisa_mem_strat */
#endif	/* ISA_ENAB */

#ifdef	MCA_ENAB
/******************************************************************************

	MCA_BRD_FOUND

	Examine MCA "slot" for an Equinox board.  If not found return false.
	If found, fill "regs" from the POS registers.  Test that the board is
	enabled.  If so, return true, else -1.  If an IRQ is enabled, 
	return -2.

	As additional board types are added, this function MAY have to be 
	updated.

	The type of "regs" is expected to be uchar_t.
	The length of "regs" is expected to be MCA_REGS_LEN.
	The contents of "regs" is defined as follows:

	regs[0]		POS0
	regs[1]		POS1
	regs[2]		POS2
	regs[3]		POS3
	regs[4]		POS4
	regs[5]		POS5
	regs[6]		unused
	regs[7]		base i/o + 0	board status
	regs[8]		base i/o + 1	software id/rev (BBBBBRRR)
	regs[9]		software created IRQ level
	regs[10]	lsb of 16-bit i/o base address
	regs[11]	msb of 16-bit i/o base address

******************************************************************************/

static int mca_brd_found( int slot, uchar_t *regs)
{
ushort_t mca_id;
uchar_t b;
ushort_t w;
uchar_t *regs_org;

  outb(0xff,0x94);  			/* enable pos access */
  outb(0x8 | slot,0x96);               /* select slot */
  mca_id = (inb(0x100) | (inb(0x101) << 8));
  if ( (mca_id != MCA_EQX_128)
          && (mca_id != MCA_EQX_64)
          && (mca_id != MCA_EQX_8) )
  {
    outb(0x0, 0x96);             /* de-select slot */
    return false;
  }

  /* fill in remainder of regs */
  regs_org = regs;
  *regs++ = mca_id & 0x00ff;
  *regs++ = (unsigned)(mca_id & 0xff00) >> 8;
  *regs++ = inb(0x102);
  *regs++ = inb(0x103);
  *regs++ = inb(0x104);
  *regs++ = inb(0x105);
  outb(0x0,0x96 );             /* de-select slot */

  if ( !(regs_org[2] & 0x01) )  /* enabled? */
    return -1;

  /* compute base i/o address */
  w = mca_iobase[ (regs_org[5] >> 3) & 0x0f ];
  * (ushort_t *) &regs_org[10] = w;

  regs_org[7] = inb( w );
  regs_org[8] = inb( w + 1 );

  /* compute irq level */
  b = cmn_irq[ regs_org[5] & 0x07 ];
  regs_org[9] = b;

  if ( regs_org[9] == 0xff )
    return true;
  else
    return -2;
}  /* mca_brd_found */

/******************************************************************************

	MCA_BASE_PADDR

	Return the mapped physical address for the board.  "regs" contains
	the array of Equinox registers.

	As additional board types are added, this function must be updated.

******************************************************************************/

static paddr_t mca_base_paddr( uchar_t *regs )
{
paddr_t p = (paddr_t)0;
uint_t w;

  w = regs[4] << 24;
  w |= regs[3] << 16;
  w |= (regs[2] & 0xc0) << 8;
  p = (paddr_t) w;
  return p;
}  /* mca_base_paddr */

/******************************************************************************

	MCA_MEM_SIZE

	Based on "req", return either the amount of ram on the card
	OR the size of the memory map required by the board (regs and
	ram) described by "regs".

		req == 0	return the size of the memory map footprint
		req == 1	return the amount of ram on the card

	As additional board types are added, this function must be updated.

	Assumes "MAX1" bit is on when appropriate.

******************************************************************************/

static uint_t mca_mem_size( uchar_t *regs, int req )
{
uchar_t b;

  if ( !req )
  {
    /* return size of footprint */
    b = *(regs + 2);
    switch ( regs[8] & 0xf8 )
    {
      case 0x10:
        if ( b & 0x02 )
          return FLAT128_MEM_LEN;
        else
          return WIN16_MEM_LEN;
      case 0x08:
        if ( b & 0x02 )
          return FLAT64_MEM_LEN;
        else
          return WIN16_MEM_LEN;
      case 0x30:
      case 0x38:
      case 0x18:
      case 0x20:
        if ( b & 0x02 )
          return FLAT8_MEM_LEN;
        else
          return WIN16_MEM_LEN;
      default:
	return 0;
    }
  }
  else  /* req == 1 */
  {
    /* return size of RAM */
    switch ( regs[8] & 0xf8 )
    {
      case 0x10:
        return 0x100000;  /* 1 meg */
      case 0x08:
        return 0x80000;   /* 1/2 meg */
      case 0x30:
      case 0x38:
      case 0x18:
      case 0x20:
	return 0x8000;    /* 32k */
      default:
	return 0;
    }
  }
}  /* mca_mem_size */

/******************************************************************************

	MCA_MEM_STRAT

	Return the memory access strategy required by the board
	described by "regs".

	As additional board types are added, this function must be updated.

******************************************************************************/

static uint_t mca_mem_strat( uchar_t *regs )
{
uchar_t b;

  b = *(regs + 2);
  if ( b & 0x02 )
    return HA_FLAT;
  else
    return HA_WIN16;
}  /* mca_mem_strat */

#endif /* MCA_ENAB */

/****************************************************************************/        
/*
** megamodem(d, cmd)
**
** mpdev board lock ** MUST ** be held			 
*/
static int megamodem(int d, int cmd)
{
	register struct mpchan *mpc = &meg_chan[d];
	register icpiaddr_t icpi = mpc->mpc_icpi;
	icpbaddr_t icpb;
	ushort_t cur, mux;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in megamodem()\n");
	}
#endif	/* DEBUG_LOCKS */

	icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
  	if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  		frame_wait(mpc,2); /* make sure regs are valid */ 
        GET_CTRL_SIGS(mpc, cur);
	mux = 0x8000;
	switch ( cmd ) {
	case TURNON:
      /* raise specific outbound signals */
      mpc->mpc_flags |= MPC_MODEM;
      /* mikes */
      /* default sigs for HW flow control off */
      cur |= (TX_DTR | TX_RTS | TX_HFC_DTR | TX_HFC_RTS);

      /* now check for HW flow control */
      if (mpc->mpc_param & IOCTRTS)
          cur &= ~TX_HFC_RTS;
      if (mpc->mpc_tty) {
          if (mpc->mpc_tty->termios)
              if (mpc->mpc_tty->termios->c_cflag & CRTSCTS)
                  cur &= ~TX_HFC_RTS;
      }
	      
      if((rx_ctrl_sig & 0x2020) == 0x2020)
         cur |= mux;
      else
         cur &= ~mux;
	/* Set the control signals "mux" bits for the SST-16I and SST-16P */
	if (mpc->mpc_mpd->mpd_board_def->number_of_ports == 16)
		cur |= 0x44;
      cur ^= TX_SND_CTRL_TG;
      SET_CTRL_SIGS(mpc, cur);
      break;

    case TURNOFF:
      /* lower specific outbound signals */
      mpc->mpc_flags &= ~MPC_MODEM;
      cur &= ~(TX_HFC_DTR | TX_HFC_RTS | TX_DTR|TX_RTS);
      cur ^= TX_SND_CTRL_TG;
      SET_CTRL_SIGS(mpc, cur);
      break;

    default:
      break;
  }

  /* return current state of DCD */
  return ((icpb->ssp.bank_sigs & (unsigned) CIN_DCD) >> 1);
} 


/*
** megaparam(chan)
**
** Map unix termio parameters into megaport
** icp channel control register parameters.
**
** mpdev board lock ** MUST ** be held			 
*/
static int megaparam( int chan) {
register struct mpchan *mpc;
volatile register struct termios *tiosp;
volatile icpiaddr_t icpi;
volatile icpoaddr_t icpo;
unsigned speed;
unsigned char oldreg;
int ii, rslt = 0;
ushort d, e;

  /* setup pointers for general use */
  mpc = &meg_chan[chan];
  if (mpc->mpc_tty == (struct tty_struct *) NULL)
	return(0);
  tiosp = mpc->mpc_tty->termios;

#ifdef	DEBUG_LOCKS
   if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in megaparam()\n");
   }
#endif	/* DEBUG_LOCKS */

   if(mpc->mpc_param & IOCTLCK){
      return(0);
   }

   /* RAMP START --------------------------------------------- */
   if (ISRAMP(mpc)){
	ramparam(mpc);
	return(0);
   }
   /* RAMP END ----------------------------------------------- */

  icpi = mpc->mpc_icpi;
  icpo = mpc->mpc_icpo;
  /* CLOCAL and carrier detect parameters */
  if (tiosp->c_cflag & CLOCAL){
    mpc->mpc_icpi->ssp.cin_attn_ena &= ~ENA_DCD_CNG;
		mpc->carr_state = 1;
  }
  else{  /* CLOCAL clear */
    /* set DCD processing */
    mpc->mpc_icpi->ssp.cin_attn_ena |= ENA_DCD_CNG;
  }

  /* outbound control signals */
  if ((tiosp->c_cflag & CBAUD) == 0)  /* B0 */
  {
    (void) megamodem(chan, TURNOFF);
    return 0;
  } else {
    mpc->carr_state = megamodem(chan, TURNON); /*shashi 02/04/98*/
  }

  /* set output control signal flow control */
  if(mpc->mpc_param & IOCTCTS) 
     tiosp->c_cflag |= CRTSCTS;
  
  /* set input control signal flow control */
  if(mpc->mpc_param & IOCTRTS) 
     tiosp->c_cflag |= CRTSCTS;

#ifdef RS422
   /* ignore flow control setting on rs-422 type of ports */

   if (mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id != LMX_8E_422 &&
       mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id != LMX_PM16_422)
   {
#endif

	 /* check for board with limited control signals */
	 if (mpc->mpc_mpd->mpd_board_def->flags & CTL2)
         {
#if	(LINUX_VERSION_CODE < 132608)
	   /* 2.2 and 2.4 kernels */
	   if (mpc->flags & ASYNC_CALLOUT_ACTIVE){
               if(tiosp->c_cflag & CRTSCTS)  /* only need to change the mask */
                 icpi->ssp.cin_susp_output_lmx |= DCD_OFF;
               else
                  icpi->ssp.cin_susp_output_lmx &= ~DCD_OFF;
            }
#endif
         }
         else
         {
            if(tiosp->c_cflag & CRTSCTS)  /* only need to change the mask */
               icpi->ssp.cin_susp_output_lmx |= CTS_OFF;
            else
               icpi->ssp.cin_susp_output_lmx &= ~CTS_OFF;
         }

#ifdef RS422
    }
#endif


#ifdef RS422
   /* ignore flow control setting on rs-422 type of ports */

   if (mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id != LMX_8E_422 &&
       mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_id != LMX_PM16_422)
   {
#endif
	 if (mpc->mpc_mpd->mpd_board_def->flags & CTL2)
         {
#if	(LINUX_VERSION_CODE < 132608)
	    /* 2.2 and 2.4 kernels */
	    if (mpc->flags & ASYNC_CALLOUT_ACTIVE){
                 if (tiosp->c_cflag & CRTSCTS)
                        tx_ctrl_sig &= ~TX_HFC_DTR;
                 else
                      tx_ctrl_sig |= TX_HFC_DTR;
            }
#endif
        }
        else
        {
              if (tiosp->c_cflag & CRTSCTS)
                    tx_ctrl_sig &= ~TX_HFC_RTS;
              else
                    tx_ctrl_sig |= TX_HFC_RTS;
        }

#ifdef RS422
    }
#endif

  /* set output inband flow control */
  if(tiosp->c_iflag & IXON || mpc->mpc_param & IXONSET) {
    unsigned short char_ctrl = icpi->ssp.cin_char_ctrl;
    if(mpc->mpc_param & IOCTXON) { 
      char_ctrl &= ~EN_DNS_FLW;  /* do not discard xon/xoff */
      char_ctrl |= EN_XON|EN_XOFF;
    }
    else /* discard xon/xoff */
      char_ctrl |= EN_XON|EN_XOFF|EN_DNS_FLW;
    char_ctrl &= ~EN_DBL_FLW; /* disable double flow */
    if (( tiosp->c_iflag & IXANY ) && !(mpc->mpc_param & IXANYIG)) 
      char_ctrl |= EN_IXANY;
    else
      char_ctrl &= ~EN_IXANY;
    cur_chnl_sync( mpc );
    mpc->mpc_stop = tiosp->c_cc[VSTOP];
    mpc->mpc_start = tiosp->c_cc[VSTART];
    icpi->ssp.cin_xoff_1 = mpc->mpc_stop;
    icpi->ssp.cin_xon_1 = mpc->mpc_start;
    icpi->ssp.cin_char_ctrl = char_ctrl;
    icpi->ssp.cin_locks &= ~DIS_IBAND_FLW;  /* clear lock bit - enable inband */
  }
  else  /* cancel action */
  {
    icpi->ssp.cin_char_ctrl &= ~EN_DNS_FLW;  /* discard xon/xoff */
    icpi->ssp.cin_locks |= DIS_IBAND_FLW;     /* set lock bit - disable inband */
    cur_chnl_sync( mpc );
    icpi->ssp.cin_inband_flow_ctrl = 0;  /* calibrate */
  }
  
  /* set input inband flow control */
  if ( tiosp->c_iflag & IXOFF ){
    mpc->mpc_stop = tiosp->c_cc[VSTOP];
    mpc->mpc_start = tiosp->c_cc[VSTART];
    icpo->ssp.cout_xoff_1 = mpc->mpc_stop;
    icpo->ssp.cout_xon_1 = mpc->mpc_start;
    icpo->ssp.cout_flow_cfg &= ~TX_XON_DBL;
    icpo->ssp.cout_flow_cfg |= TX_XON_XOFF_EN;
  }
  else  /* cancel enable, send XON if we previously sent an XOFF */
  {
	 if ((icpo->ssp.cout_flow_cfg & TX_XON_XOFF_EN) && (icpi->ssp.cin_int_flags & 0x40)){
		megajam(mpc, mpc->mpc_start);
	}
    icpo->ssp.cout_flow_cfg &= ~(TX_XON_XOFF_EN|TX_XON_DBL);
  }
  
  /* baudrate */
  speed = tiosp->c_cflag & CBAUD;
#ifdef DEBUG
  printk("speed for device %d is %d\n", chan, speed);
#endif
#ifdef SPD
  ispeed = (tiosp->c_cflag & CIBAUD) >> IBSHIFT;
  if ( !ispeed )
  {
    ispeed = speed;
    /* set CIBAUD to match CBAUD since we support split speeds */
    tiosp->c_cflag |= (ispeed << IBSHIFT);
  }
#endif
  if (speed & CBAUDEX){
	speed &= ~CBAUDEX;
	if ((speed < 1) || (speed > 4))
		tiosp->c_cflag &= ~CBAUDEX;
	else 
		speed += 15;
  }
  if ( speed >= 0 && speed <= (B38400 + 4)) {
	icpqaddr_t icpq; /* 1.07 */
    d = icpbaud(speed, mpc);
    if (d != icpi->ssp.cin_baud)
       icpi->ssp.cin_baud = d;
    if (d != icpo->ssp.cout_baud) {
      icpo->ssp.cout_baud = d;
	  icpq = &icpo->ssp.cout_q0; /* 1.07 */
	
      cur_chnl_sync(mpc);
		if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
			/* SSP64 */
	    		icpq->ssp.q_block_count = lowat[speed];
		else
			/* SSP2 / SSP4 */
			icpq->ssp.q_block_count = ssp4_lowat[speed];
#ifdef DEBUG
    printk("speed for for device %d is %d\n", chan, speed);
#endif
	if (speed == B38400){
		if ((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI){
			if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
				/* SSP64 */
	    			icpq->ssp.q_block_count = lowat[speed + 1];
			else
				/* SSP2 / SSP4 */
				icpq->ssp.q_block_count = ssp4_lowat[speed + 1];
		}
		else if ((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI){
			if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
				/* SSP64 */
	    			icpq->ssp.q_block_count = lowat[speed + 2];
			else
				/* SSP2 / SSP4 */
				icpq->ssp.q_block_count = ssp4_lowat[speed + 2];
		}
	}
      icpo->ssp.cout_int_baud_ctr = 0;
      cur_chnl_sync(mpc);
      if (icpo->ssp.cout_int_baud_ctr)
        icpo->ssp.cout_int_baud_ctr = 0;
    }
    if ( d >= 0x7ffe )
      icpo->ssp.cout_flow_cfg |= TX_XTRA_DMA;
    else
      icpo->ssp.cout_flow_cfg &= ~TX_XTRA_DMA;
  } else
    	rslt = 1;  /* no valid speed to set */

  /* databits */
  e = icpi->ssp.cin_char_ctrl &= ~0x2000;  /* default to no discard bit7 */
  switch (tiosp->c_cflag & CSIZE)
  {                                    /* set character size */
    case CS5:
      d = 0x00;
      break;

    case CS6:
      d = 0x01;
      break;

    case CS7:
      d = 0x02;
      break;

    default:  /* case CS8 - no other value should occur */
      d = 0x03;
      if(tiosp->c_iflag & ISTRIP)
        e |= 0x2000;  /* discard bit 7 */
  }
  if(tiosp->c_cflag & PARENB)
  {
    d |= 0x04;
    if( !(tiosp->c_cflag & PARODD) )
      d |= 0x08;
  }
  e &= ~0x001f;
  oldreg = tx_cpu_reg;
  tx_cpu_reg |= TX_SUSP;
  cur_chnl_sync(mpc);
  icpi->ssp.cin_char_ctrl = (d | e);
  e = icpo->ssp.cout_char_fmt;
  e &= ~0x001f;
  icpo->ssp.cout_char_fmt = (d | e);
  if (!(oldreg & TX_SUSP))
    tx_cpu_reg &= ~TX_SUSP;

  /* prepare for input break/parity processing - cancel special features */
  icpi->ssp.cin_char_ctrl &= ~0x03e0;
  mpc->mpc_cin_stat_mask &= ~0x03a0;

  /* prepare for input break processing */
  if ( tiosp->c_iflag & IGNBRK ) 
  {
    /* hardware can ignore break nulls */
    icpi->ssp.cin_char_ctrl |= IGN_BRK_NULL;
  }
  else
  {
    /* always ignore break, break event handled in megasint */	  
    icpi->ssp.cin_char_ctrl |= IGN_BRK_NULL;
    mpc->mpc_cin_stat_mask |= 0x0120;   /* watch for framing errs w/nulls */
  }
  rx_cie |= mpc->mpc_cin_stat_mask;
  /* clear lookup table - processing flags already clear */
  for ( ii = 0; ii < 32; ii++ )
    icpi->ssp.cin_lookup_tbl[ii] = 0;

  /* prepare for input parity processing */
  if ( tiosp->c_cflag & PARENB
	&& tiosp->c_iflag & INPCK )
  {
    if ( tiosp->c_iflag & IGNPAR )
    {
      /* discard chars with parity/framing errors */
      icpi->ssp.cin_char_ctrl |= IGN_BAD_CHAR;
    }
    else
    {
      /* hardware must maintain and tag err'd chars */
      mpc->mpc_cin_stat_mask |= 0x0180; /* watch for parity and framing */
      if ( tiosp->c_iflag & PARMRK 
              && !(tiosp->c_iflag & ISTRIP) )
      {
	/* put 0xff in lookup table */
	icpi->ssp.cin_lookup_tbl[0x1f] |= 0x80;
	icpi->ssp.cin_char_ctrl |= EN_CHAR_LOOKUP;  /* enable lookup */
        mpc->mpc_cin_stat_mask |= 0x0200;     /* watch for lookup event */
     }
    }
  }
  /* output stop bits */
  if (tiosp->c_cflag & CSTOPB)
    icpo->ssp.cout_char_fmt |= 0x20;
  else
    icpo->ssp.cout_char_fmt &= ~0x20;
    icpo->ssp.cout_ses_ctrl_a = 0;  /* default */

  if ( tiosp->c_cflag & CREAD )
    /* make sure dma's to dram are enabled */
    icpi->ssp.cin_locks &= ~0x80;
  else
    /* disable unnecessary dma's to dram */
    icpi->ssp.cin_locks |= 0x80;
  return rslt;
}   

/*
** icpbaud(val, mpc)
**
** Compute the 15-bit baud value for "cin_baud" or "cout_baud".  The
** input is the the current strtty "t_cflag & CBAUD".  The 16th bit
** (activate autobaud) is always returned false.
**
** There are certain "unitcode" restrictions which are not reflected
** in this code.
**
** mpdev board lock ** MUST ** be held			 
*/
static  ushort_t icpbaud( int val, struct mpchan *mpc)
{
int baud, maxbaud;
#ifdef DEBUG
   printk("icpbaud: device %d, val = %d\n", mpc->mpc_chan, val);
#ifdef DEBUG_LOCKS
   if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in icpbaud()\n");
   }
#endif	/* DEBUG_LOCKS */
#endif	/* DEBUG */

   switch(mpc->mpc_icp->lmx[mpc->mpc_lmxno].lmx_speed)
   {
   case 0:
	maxbaud = 115200;
	break;
   case 1:
	maxbaud = 230400;
	break;
   case 2:
	maxbaud = 460800;
	break;
   case 3:
	maxbaud = 921600;
	break;
   default:
	maxbaud = 115200;
	break;
   }

  baud = icpbaud_tbl[val];
  if (baud == 38400){
  	if((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
		baud = 57600;
  	else if((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
		baud = 115200;
  }
  if(baud == 2 * maxbaud / 3)
	return(0x7ffe);
  
  return (~(2*maxbaud/baud - 2) & 0x7fff) | 1;
 
}  /* icpbaud */

/*
** chanon(mpc)
**
** Turn on a channel.
**
** mpdev board lock ** MUST ** be held			 
*/
static int chanon( struct mpchan *mpc)
{
  volatile register icpiaddr_t icpi = mpc->mpc_icpi;
  volatile register icpoaddr_t icpo = mpc->mpc_icpo;
  volatile register icpgaddr_t icpg = (icpgaddr_t)icpo;
  volatile icpqaddr_t icpq = &icpo->ssp.cout_q0;
  volatile icpbaddr_t icpb;
  unsigned short cur;
  uchar_t cin;

#ifdef	DEBUG_LOCKS
  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
	printk("LOCK Failure: mpd board lock NOT locked in chanon()\n");
  }
#endif	/* DEBUG_LOCKS */

  if (mpc->mpc_mpd->mpd_board_def->asic != SSP64)  /* SSP2 / SSP4 only */
	icpg = (icpgaddr_t)((unsigned long)(mpc->mpc_icpi) + 0x400);
  icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
  if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  	frame_wait(mpc,2); /* make sure regs are valid */ 
  if((rx_ctrl_sig & 0x20a0) == 0x20a0) {	/* if AMI CFG */
    icpi->ssp.cin_susp_output_lmx |= 0x20; /* susp on mux disconnect */ 
    tx_lmx_cmd |= 0x80;		/* in case of a mux clear loopback */
  }
  else {
    if (!(ISRAMP(mpc)))
          /* local panel */
    	icpi->ssp.cin_susp_output_lmx &= ~0x20;
    tx_lmx_cmd &= ~0x80;
  }
  tx_lck_ctrl |= LCK_Q_ACT;
  cin = icpi->ssp.cin_locks;
  icpi->ssp.cin_locks = cin | 0x03;
  cur_chnl_sync( mpc );

  icpi->ssp.cin_bank_a.ssp.bank_fifo_lvl &= ~0x8f;
  icpi->ssp.cin_bank_b.ssp.bank_fifo_lvl &= ~0x8f;
  icpi->ssp.cin_tail_ptr_a = 
  icpi->ssp.cin_tail_ptr_b =  
  mpc->mpc_rxq.q_ptr = rx_next;
  if(mpc->mpc_mpd->mpd_nwin)
     mpc->mpc_rxq.q_ptr &= 0x3fff;
  rx_vmin = 1;
  /* unlock input */
  icpi->ssp.cin_attn_ena |= CIN_DEF_ENABLES;
  icpi->ssp.cin_locks = cin;
  tx_dma_stat = 0;
  icpo->ssp.cout_int_fifo_ptr = 0;  /* clr internal fifo ptr */
  Q_data_count = 0;
  Q_data_ptr = mpc->mpc_txbase + mpc->mpc_txq.q_begin;
  icpi->ssp.cin_inband_flow_ctrl = 0;  /* calibrate */
  mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
  mpc->mpc_count = 0;
  icpo->ssp.cout_status &= ~0x02;  /* clear sending break state */
  GET_CTRL_SIGS(mpc, cur);
  cur |= TX_HFC_DTR|TX_HFC_RTS;
  SET_CTRL_SIGS(mpc, cur);

  /* unlock output */
  icpo->ssp.cout_attn_ena |= COUT_DEF_ENABLES;  /* includes 0x8000 */
  tx_lck_ctrl &= ~LCK_Q_ACT;

  /* calibrate invent char count */
  mpc->mpc_tx_last_invent = icpo->ssp.cout_ses_invent_a;

  /* Calibrate events and control signals in both banks */
  FREEZ_BANK(mpc);
  FREEZ_BANK(mpc);
  mpc->mpc_cin_events = 0;

  mpc->mpc_flags |= MPC_OPEN;
  return(0);
}   /* chanon */

/*
** chanoff(mpc)
**
** Turn off a channel.
**
** mpdev board lock ** MUST ** be held			 
*/
static int chanoff( struct mpchan *mpc)
{
  volatile register icpiaddr_t icpi;
  volatile register icpoaddr_t icpo;
  icpqaddr_t icpq;
  volatile icpbaddr_t icpb;
  int loop;
  int ok;
  uchar_t cin;
  ushort_t events;

#ifdef	DEBUG_LOCKS
  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
	printk("LOCK Failure: mpd board lock NOT locked in chanoff()\n");
  }
#endif	/* DEBUG_LOCKS */

  icpi = mpc->mpc_icpi;
  icpo = mpc->mpc_icpo;
  icpq = &icpo->ssp.cout_q0;
	icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
  if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  	frame_wait(mpc,2); /* make sure regs are valid */ 

  mpc->mpc_param &= (IOCTLLB|IOCTCTS|IOCTLCK|IOCTRTS);
  /* disable events */
  icpi->ssp.cin_attn_ena &= (0x1e);
  if (!(mpc->mpc_chan % 16)) /* Shashi 04/01/98 */
  icpi->ssp.cin_attn_ena |= ENA_LMX_CNG;

  tx_lck_ctrl |= LCK_Q_ACT;
  cur_chnl_sync( mpc );
  icpo->ssp.cout_int_fifo_ptr = 0;  /* clr internal fifo ptr */
  tx_dma_stat = 0;
  Q_data_count = 0;
  Q_data_ptr = mpc->mpc_txbase + mpc->mpc_txq.q_begin;
  mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
  mpc->mpc_count = 0;
  mpc->carr_state = 0;
  tx_lck_ctrl &= ~LCK_Q_ACT;
  cur_chnl_sync( mpc );
  if ((icpo->ssp.cout_int_save_togl & 0x4) == (icpo->ssp.cout_cpu_req & 0x4)) 
    tx_cpu_reg ^= CPU_SND_REQ;

  /* wait for ack in chanoff */
  loop = 0;
  ok = false;
  while ( ++loop < 100000 )
  {
    if ( icpo->ssp.cout_status & 0x10 )
      events = icpo->ssp.cout_events_b;
    else
      events = icpo->ssp.cout_events_a;
    if ( events & 0x01 )
    {
      ok = true;
      break;
    }
  }
  if ( !ok )
    printk("WARNING: SST: cpu_req ack failed (chanoff).\n" );

  cin = icpi->ssp.cin_locks;
  icpi->ssp.cin_locks = cin | 0x03;
  cur_chnl_sync( mpc );
  icpi->ssp.cin_bank_a.ssp.bank_fifo_lvl &= ~0x8f;
  icpi->ssp.cin_bank_b.ssp.bank_fifo_lvl &= ~0x8f;
  icpi->ssp.cin_tail_ptr_a = 
  icpi->ssp.cin_tail_ptr_b =  
  mpc->mpc_rxq.q_ptr = rx_next;
  if(mpc->mpc_mpd->mpd_nwin)
     mpc->mpc_rxq.q_ptr &= 0x3fff;
  rx_vmin = 1;
  /* unlock input */
  icpi->ssp.cin_attn_ena |= /*CIN_DEF_ENABLES*/ 0x2800; /* To handle the overrun 
							 when closed
							*/
  icpi->ssp.cin_locks = cin;
  if ( icpi->ssp.cin_inband_flow_ctrl & 0x01 )
  {
    icpi->ssp.cin_locks |= 0x08;
    cur_chnl_sync( mpc );
    icpi->ssp.cin_inband_flow_ctrl &= ~0x01;
    icpi->ssp.cin_locks &= ~0x08;
  }
  icpo->ssp.cout_attn_ena = 0x8000;  /* allow update enabled */
  mpc->mpc_flags &= ~MPC_OPEN;
  mpc->mpc_cin_events = 0;
  mpc->mpc_cout_events = 0;
  return(0);
}   /* chanoff */


/*
** cur_chnl_sync(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
static int cur_chnl_sync( register struct mpchan *mpc)
{
register icpgaddr_t icpg = (icpgaddr_t)mpc->mpc_icpo;
register int i = 0;
volatile unsigned char *chan_ptr;
	
#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in cur_chnl_sync()\n");
	}
#endif	/* DEBUG_LOCKS */

	if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)  /* SSP64 board */
		chan_ptr = &(Gicp_chnl);
	else { /* SSP4 board */
      		icpaddr_t icp;
      		volatile union global_regs_u *icp_glo ;
      		icp = mpc->mpc_icp;
      		icp_glo = (volatile union global_regs_u *)((unsigned long)icp->icp_regs_start + 0x400);
      		chan_ptr = &(icp_glo->ssp2.chan_ctr);
	}
	while(*chan_ptr == mpc->mpc_chan) { if( ++i > 9000) break; }
	return(0);
}

/*
** mega_push_winicp(mpd, icp)
**
** mpdev board lock ** MUST ** be held			 
*/
static int mega_push_winicp( struct mpdev *mpd, int icp)
{
#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in mega_push_winicp()\n");
	}
#endif	/* DEBUG_LOCKS */

	if(wtos) {
          /* deselect current before push, if necessary */
          if (wstk[wtos].ws_mpd->mpd_board_def->asic == SSP64)
	    /* SSP64 */
	    outw(0,wstk[wtos].pgctrl);
           else 
            /* SSP2 / SSP4 */
            outb(0, wstk[wtos].pgctrl);
        }

	if(++wtos >= WINDO_DEPTH) {
	   printk("win16: stk overflow - %d\n",wtos);
	   wtos--;
	   return(1);
	}
        wstk[wtos].ws_mpd = mpd;
        if (mpd->mpd_board_def->asic == SSP64)
		/* SSP64 */
		outw((wstk[wtos].pgdata = (icp | 0x100)),
			(wstk[wtos].pgctrl = mpd->mpd_pgctrl));
        else
		/* SSP2 / SSP4 */
                outb((wstk[wtos].pgdata = (1 << icp)),
                	(wstk[wtos].pgctrl = mpd->mpd_pgctrl));
	return(0);
}

/*
** mega_push_win(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
static int mega_push_win( struct mpchan *mpc, int typ)
{
	struct mpdev *mpd = mpc->mpc_mpd;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in mega_push_win()\n");
	}
#endif	/* DEBUG_LOCKS */

	if(wtos) {
          /* deselect current before push, if necessary */
          if(wstk[wtos].ws_mpd->mpd_board_def->asic == SSP64)
	    /* SSP64 */
	    outw(0,wstk[wtos].pgctrl);
           else 
	    /* SSP2 / SSP4 */ 
            outb(0, wstk[wtos].pgctrl);
        }
	if(++wtos >= WINDO_DEPTH) { 
	   printk("win16: stk overflow - %d\n",wtos);
	   wtos--;
	   return(1);
	}

        wstk[wtos].ws_mpd = mpd;
        if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
	  switch(typ) {
	    case 0:
		outw((wstk[wtos].pgdata = mpc->mpc_icpno | 0x100),
		(wstk[wtos].pgctrl = mpc->mpc_mpd->mpd_pgctrl));
		break;
	    case 1:
		outw((wstk[wtos].pgdata = mpc->mpc_rxpg | 0x300),
		(wstk[wtos].pgctrl = mpc->mpc_mpd->mpd_pgctrl));
		break;
	    case 2:
		outw((wstk[wtos].pgdata = mpc->mpc_txpg | 0x300),
		(wstk[wtos].pgctrl = mpc->mpc_mpd->mpd_pgctrl));
		break;
	    case 3:
		outw((wstk[wtos].pgdata = mpc->mpc_tgpg | 0x200),
		(wstk[wtos].pgctrl = mpc->mpc_mpd->mpd_pgctrl));
		break;
	  }
        }
        else { /* SSP4 board */
		outw((wstk[wtos].pgdata = (1 << mpc->mpc_icpno)),
		(wstk[wtos].pgctrl = mpc->mpc_mpd->mpd_pgctrl));
        }
	return(0);
}

/*
** mega_pop_win(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
static int mega_pop_win(int instance)
{
	if ((wtos == 0) && (last_pop)) { 
		last_pop = 0;

#ifdef	DEBUG
printk("wtos_max %d EXTRA POP on %d last was %d %d %d %d %d %d %d\n",wtos_max,instance,last_pop1,last_pop2,last_pop3,last_pop4,last_pop5,last_pop6,last_pop7);
#endif	/* DEBUG */

	}
	
#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&wstk[wtos].ws_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in mega_pop_win()\n");
	}
#endif	/* DEBUG_LOCKS */

	last_pop7 = last_pop6;
	last_pop6 = last_pop5;
	last_pop5 = last_pop4;
	last_pop4 = last_pop3;
	last_pop3 = last_pop2;
	last_pop2 = last_pop1;
	last_pop1 = instance;
	if(wtos) {
          /* deselect current before pop, if necessary */
          if(wstk[wtos].ws_mpd->mpd_board_def->asic == SSP64) 
	    /* SSP64 */
	    outw(0,wstk[wtos].pgctrl);
          else 
	    /* SSP2 / SSP4 */
            outb(0, wstk[wtos].pgctrl);
        }
	if(!(--wtos)) {
	   return(1);
	}
	if(wtos < 0) {

#ifdef	DEBUG
	   printk("win16: stk underflow - %d \n",wtos);
           printk("wtos_max %d EXTRA POP on %d last was %d %d %d %d %d %d %d\n",wtos_max,instance,last_pop1,last_pop2,last_pop3,last_pop4,last_pop5,last_pop6,last_pop7);
#endif	/* DEBUG */

	   wtos = 0;
	   return(1);
	}
        if (wstk[wtos].ws_mpd->mpd_board_def->asic == SSP64)
	  /* SSP64 */
	  outw(wstk[wtos].pgdata, wstk[wtos].pgctrl);
        else
	  /* SSP2 / SSP4 */
	  outb((unsigned char)wstk[wtos].pgdata, wstk[wtos].pgctrl);
	return(0);
}
	
/*
** frame_wait(mpc, count)
** on "mcp" wait at least "count" frames to elapse.
**
** mpdev board lock ** MUST ** be held			 
*/
static int frame_wait( register struct mpchan *mpc, int count)
{
  icpgaddr_t icpg = (icpgaddr_t)(mpc->mpc_icpo);
  /* ssp2_output_s pointer */
  icpoaddr_t icpo = (icpoaddr_t)((unsigned long)
	(mpc->mpc_icp->icp_regs_start) + 0x200);
  ushort_t final,original,current1;
  volatile ushort_t *frame_ptr;
  int x = 0;

#ifdef	DEBUG_LOCKS
  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in frame_wait()\n");
  }
#endif	/* DEBUG_LOCKS */

  if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
    /* SSP64 */
    frame_ptr = &Gicp_fram_ctr;
  else {
    /* SSP2 / SSP4 */
    icpg = (icpgaddr_t)((unsigned long)(mpc->mpc_icpi) + 0x400);
    frame_ptr = &(icpo->ssp2.frame_ctr);
  }
  original = *frame_ptr;
  final = original + count;
  if ( final < (unsigned) count ) {  /* wrap */
    while (1) {
      if ( (current1 = *frame_ptr) < original && current1 > final )
        break;
      if ( ++x > 0x100000 )
      {
  	       printk("WARNING: SST: frame_wait error\n");
        break;
      }
    }
  } /* Wrap loop */
  else { /* Non-wrap */
    while (1) {
      if ( (current1 = *frame_ptr) > final || current1 < original )
        break;
      if ( ++x > 0x100000 )
      {
  	       printk("WARNING: SST: frame_wait error\n");
        break;
      }
    }
  } /* Non-wrap loop */
  return(0);
}  /* frame_wait */

/*
** frame_ctr_reliable(icpg, mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
static int frame_ctr_reliable( icpgaddr_t icpg, register struct mpchan *mpc)
{
int ii = 0;
uchar_t w;
ushort_t x, b; 
volatile ushort_t *frame_ptr;

#ifdef	DEBUG_LOCKS
  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in frame_ctr_reliable()\n");
  }
#endif	/* DEBUG_LOCKS */

  if (mpc->mpc_mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
    w = Gicp_chnl;
    x = Gicp_fram_ctr;
    frame_ptr = &(Gicp_fram_ctr);
  }
  else { /* SSP4 board */
    icpgaddr_t icpg = (icpgaddr_t)((unsigned long)(mpc->mpc_icp->icp_regs_start) + 0x400);
    icpoaddr_t icpo = (icpoaddr_t)((unsigned long)(mpc->mpc_icp->icp_regs_start) + 0x200);
    w = icpg->ssp2.chan_ctr;
    x = icpo->ssp2.frame_ctr;
    frame_ptr = &(icpo->ssp2.frame_ctr);
  }
  if ( w & 0xc0 )  /* impossible channel number */
    return false;

  while (( b = *frame_ptr) == x )
    if ( ++ii > 0x10000 )
      return false;
  return true;
}  /* frame_ctr_reliable */

/*
** megajam(mpc, c)
**
** Jam a character into the output queue.  If the transmitter
** is idle, it's easy: just place the character in the output
** queue and call megastart().  In the more difficult case,
** we must stop the transmitter, push the character into the
** "output next byte" register, and then restart normal output.
** 
** mpdev board lock ** MUST ** be held			 
*/
static int megajam( register struct mpchan *mpc, char c)
{
  register icpoaddr_t icpo = mpc->mpc_icpo;
  volatile icpiaddr_t icpi = mpc->mpc_icpi;
  int ii=0;

#ifdef	DEBUG_LOCKS
  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
	printk("LOCK Failure: mpd board lock NOT locked in megajam()\n");
  }
#endif	/* DEBUG_LOCKS */

  if ( !(icpo->ssp.cout_flow_cfg & 0x02) )
    return(0);  /* may hang on second while */
	 if (!(icpi->ssp.cin_int_flags & 0x40))
		return(0);

  while (( (icpo->ssp.cout_flow_cfg & 0x10) != (icpo->ssp.cout_int_flow_ctrl & 0x10)) &&
        ++ii < 100000);

  ii = 0;
  while (icpo->ssp.cout_int_flow_ctrl & 0x08 && ++ii < 100000 );  /* wait here */
  if(icpo->ssp.cout_int_flow_ctrl & 0x08)
  {
             printk("WARNING: SST: send flow char ack missing - char is %d\n", c );
    return(0);
  }
  if (!(ISRAMP(mpc))) {
  if(c == icpo->ssp.cout_xoff_1)
    icpo->ssp.cout_flow_cfg &= ~0x08;
  else
    if(c == icpo->ssp.cout_xon_1)
      icpo->ssp.cout_flow_cfg |= 0x08;
    else
      return(0);
  icpo->ssp.cout_flow_cfg ^= 0x10;
  }
    return(0);
}  

/*
** eqnx_delay(len)
**
** Routine to schedule a delay. Does not busy out CPU. 
*/
static void eqnx_delay(int len)
{
#ifdef DEBUG
	printk("eqnx_delay(len=%d)\n", len);
#endif
	if (len > 0) {
#if LINUX_VERSION_CODE < 0x2017f 
		current->state = TASK_INTERRUPTIBLE;
		current->timeout = jiffies + len;
		schedule();
#else
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(len);
		current->state = TASK_RUNNING;
#endif
	}
}

/*
** eqnx_dohangup(arg)
**
** This routine is called from the scheduler tqueue when the interrupt
** routine has signalled that a hangup has occurred.  The path of
** hangup processing is:
**
** 	poll routine -> (scheduler tqueue) ->
** 	eqnx_dohangup() -> tty->hangup() -> eqnx_hangup()
** 
*/
static void eqnx_dohangup(void *arg)
{
	struct mpchan *mpc;


	mpc = (struct mpchan *) arg;
	if (mpc == (struct mpchan *) NULL)
		return;
	if (mpc->mpc_tty == (struct tty_struct *) NULL)
		return;
#ifdef DEBUG
	printk("eqnx_dohangup: device =%x\n", 
		(unsigned int) SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
	tty_hangup(mpc->mpc_tty);
}

/*
** sst_poll(arg)
**
** MEGAPOLL
** Board polling routine.  Called once every MEGATIMEO
** clock ticks.  Other than input control line transitions,
** all megaport events along with input buffer not empty,
** and output buffer empty conditions are handled here.
*/

#define WATCHDOGID 0xAA /* Timeout in 16 seconds, KEY of 0x0A */

void sstpoll(unsigned long arg)
{

	register struct mpchan *mpc;
	register icpgaddr_t icpg;
	register struct mpdev *mpd;
	icpoaddr_t icpo;
	icpiaddr_t icpi;
	icpbaddr_t icpb;
	icpaddr_t  icp;
	int i, j, ii, port, nports, win16;
	uchar_t g_attn;
	uint_t rbits, xbits, rbits0, xbits0, rbits1, xbits1;
	uchar_t rxtx_work;
	unsigned char read_poll;
	volatile icpqaddr_t icpq;
	unsigned long flags;

	poll_cnt++;
	if ((poll_cnt & 7) == 0)
		read_poll = 1; /* Do read side for SSP64 boards at 80 mills */
	else
		read_poll = 0;

	for (i=0; i < (int)nmegaport; i++) {	/* adapters loop */
		mpd = &meg_dev[i];
		if (mpd->mpd_alive == 0) 
			continue; /* If board is dead skip it */

		/* Process certain SSP64 boards every 40 mills */
		if ((mpd->mpd_board_def->flags & POLL40) &&
				(poll_cnt & 3))
			continue;

		win16 = mpd->mpd_nwin;

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpd->mpd_lock, flags);

		/* icps loop */
		for(j=0; j < (int)mpd->mpd_nicps; j++) {
			mpc = mpd->mpd_mpc + j * mpd->mpd_sspchan;

			/* validity check while booting */
			if ((mpc->mpc_icpi == NULL) || (mpc->mpc_icpo == NULL))
				continue;

			if(win16)
				mega_push_winicp(mpd, j);
			icp = &mpd->icp[j];

			/* SSP64 board */
			if (mpd->mpd_board_def->asic == SSP64) {
				icpg = 
				   (icpgaddr_t)((unsigned long)mpd->icp[j].icp_regs_start
				   + 0x2000);
				g_attn = i_gicp_attn;
#ifdef DEBUG1
				printk("frame counter = %d, poll_cnt = %d\n", 
						poll_cnt, Gicp_fram_ctr);
#endif
#ifdef WDOG
				icpg->ssp.gicp_watchdog = WATCHDOGID;
#endif

				/* ATMEL: Give the ring time to settle */
				if (icp->icp_rng_state == RNG_WAIT) {  /* ATMEL */
				    /* if we lose ring clock - start over */
				    if (g_attn & RNG_FAIL) { 
					icp->icp_rng_wait = 0;
					icp->icp_rng_last = 0;
				    }
				    /* else wait 800 milliseconds */
				    else if (icp->icp_rng_wait++ < 20) {
					if (win16)
					    mega_pop_win(28);
					continue;
				    }
				    /* call mega_rng_delta with RNG_GOOD below */
				    icp->icp_rng_wait = 0; 
				}
				/* End ATMEL: Give the ring time to settle */
				/* ATMEL: Check the ring sync for 800 mills */
				if (icp->icp_rng_state == RNG_CHK) { /* ATMEL */
				    /* if we lose ring clock, start over */
				    if (g_attn & RNG_FAIL) { 
					icp->icp_rng_wait = 0;
					icp->icp_rng_last = 0;
				    } else {
					icp->icp_rng_svcreq = mega_rng_delta(icp,
						RNG_CHK, mpd->mpd_nchan);
					if (win16)  
					    mega_pop_win(28);
					continue;
				    }
				}
				/* End ATMEL: Verify ring sync for 800 mills */                      
				if((g_attn & RNG_FAIL) && !icp->icp_rng_last) {
		   			/* bit changed to on - 
					 * ring changed to "bad" */
					icp->icp_rng_svcreq = 
					   mega_rng_delta(icp, RNG_BAD, 
					   mpd->mpd_nchan);
					/* save last status */ 
					icp->icp_rng_last = g_attn & RNG_FAIL; 
				}
				else if(!(g_attn & RNG_FAIL) && 
					icp->icp_rng_last) {
					/* bit changed to off - 
					 * ring changed to "good" */

					/* ATMEL: Let the ring clock stabilize */
					if (icp->icp_rng_state == RNG_BAD) {
						icp->icp_rng_state = RNG_WAIT;
						if (win16)
							mega_pop_win(28);
						continue;
					}
					/* END ATMEL */
				
					/* save last status */
					icp->icp_rng_last = g_attn & RNG_FAIL;
					if(Gicp_initiate & RNG_CLK_ON) {
						icp->icp_rng_svcreq = 
						   mega_rng_delta(icp, RNG_GOOD,
						   mpd->mpd_nchan);
					}
					if (icp->icp_rng_state == RNG_BAD) {
						icp->icp_rng_state = RNG_WAIT;
						icp->icp_rng_wait = 0;
					}
					if (win16)
						mega_pop_win(28);
					continue;
					} else {
						/* check software reason */
          					if(!(mpc->mpc_mpa_stat & MPA_INITG_UART)) {
	   						if(icp->icp_rng_svcreq){
							  icp->icp_rng_svcreq = 
						   	   mega_rng_delta(icp, 
						   	   icp->icp_rng_svcreq, 
						   	   mpd->mpd_nchan);
							}
						} else {
          /* RAMP: When initializing a modem via ramp services, false 
             lmx condition change and control signal events can and will
             occur. They will be ignored for now. Monitor severity. */

             						MESSAGE("WARNING: Ignoring ring delta on modem", mpc->mpc_mpa_stat,mpc);
             						icp->icp_rng_svcreq = 0;
             						mpc->mpc_cin_events &= 
								~EV_LMX_CNG;
						 }
					}
#ifdef WDOG
					if(g_attn & WDOG_EXP) {
       printk("WARNING: SST watchdog timer expired, expansion bus disabled on devs %d to %d\n",
							icp->icp_minor_start,icp->icp_minor_start+MAXICPCHNL-1);
						/* Disable watchdog until next megapoll */
						icpg->ssp.gicp_watchdog = 0; 
	    					Gicp_initiate = 0x1e - 
							RNG_CLK_ON;
						if(win16)
							mega_pop_win(28);
						continue; 
					}
#endif /** if WDOG **/

					if(!(g_attn & (GATTN_RX|GATTN_TX))) {
						if(win16)
							mega_pop_win(28);
						continue; 
					}

					/* Note: if brd is ssm based 12/24 don't do odd read */
					if ((mpd->mpd_board_def->number_of_ports == 12) ||
					    (mpd->mpd_board_def->number_of_ports == 24))
					{
						/* Skip read side processing cycles */
					}

					/* allow clear when read */
					Gicp_initiate &= ~0x10;	
					if(g_attn & GATTN_RX) {
						rbits0 = * (uint_t *) &gicp_rx_attn[0];
					rbits1 = * (uint_t *) &gicp_rx_attn[4];
				}
				else 
					rbits0 = rbits1 = 0;

				/* ATMEL */
				if (icp->icp_rng_state != RNG_GOOD) { 
				    /* block clear when read */
				    Gicp_initiate |= 0x10; 
				    if (win16) 
					    mega_pop_win(28);
				    continue;
				}
				/* ATMEL */

				if(g_attn & GATTN_TX) {
					xbits0 = * (uint_t *) &gicp_tx_attn[0];
					xbits1 = * (uint_t *) &gicp_tx_attn[4];
				}
				else 
					xbits0 = xbits1 = 0;

				Gicp_initiate |= 0x10;	/* block clear when read */

				xbits = xbits0; 
				rbits = rbits0; 
				if (mpd->mpd_nchan < NCHAN) 
					nports = mpd->mpd_nchan; 
				else
					nports = NCHAN; 

				for(port = 0; port < nports; port++, mpc++) { /* ports loop */
					if(xbits & 1) {
						icpo = mpc->mpc_icpo;
						icpq = &icpo->ssp.cout_q0;
						TX_EVENTS(mpc->mpc_cout_events, mpc);
						if(mpc->mpc_cout_events & 
							EV_TX_EMPTY_Q0) {
#ifdef DEBUG1
	printk("SSP64:EMPTY_Q0 event for device %d\n", 
			mpc->mpc_chan + (i * NCHAN_BRD) + (j * NCHAN));
	icpq = &icpo->ssp.cout_q0;
	printk("Q data count %d\n", Q_data_count);
	printk("frame counter %d\n", Gicp_fram_ctr);
#endif
							if (Q_data_count)
							tx_cie |= (ENA_TX_EMPTY_Q0);
							else{
								mpc->mpc_flags &= ~MPC_BUSY;
								megatxint(mpc);

								if (write_wakeup_deferred) {
									spin_unlock_irqrestore(&mpd->mpd_lock, flags);
									write_wakeup_deferred = 0;
									mpc->mpc_tty->ldisc.write_wakeup(mpc->mpc_tty);
									spin_lock_irqsave(&mpd->mpd_lock, flags);
								}
							}
							mpc->mpc_cout_events &= 
						   		~EV_TX_EMPTY_Q0;
						}
						else if(mpc->mpc_cout_events & 
							EV_TX_LOW_Q0) {
#ifdef DEBUG
	printk("LOW_Q0 event for device %d\n", 
			mpc->mpc_chan + (i * NCHAN_BRD) + (j * NCHAN));
	icpq = &icpo->ssp.cout_q0;
	printk("Q data count for device %d\n", Q_data_count);
	printk("frame counter for device %d\n", Gicp_fram_ctr);
	printk("q block count %d\n", icpq->ssp.q_block_count);
#endif
							if (Q_data_count >=
						(icpq->ssp.q_block_count * 64))
							tx_cie |= (ENA_TX_LOW_Q0);
							else {
								megatxint(mpc);

								if (write_wakeup_deferred) {
									spin_unlock_irqrestore(&mpd->mpd_lock, flags);
									write_wakeup_deferred = 0;
									mpc->mpc_tty->ldisc.write_wakeup(mpc->mpc_tty);
									spin_lock_irqsave(&mpd->mpd_lock, flags);
								}
							}
							mpc->mpc_cout_events &= 
						   	~EV_TX_LOW_Q0;
						}
						if(mpc->mpc_cout_events & 
							EV_OUT_TMR_EXP) {
							mpc->mpc_cout_events &= 
						   	~EV_OUT_TMR_EXP;
						}
					}		
if ((mpc->mpc_mpa_stat & (MPA_INITG_INIT_MODEM | MPA_INITG_MODIFY_SETTINGS |
MPA_INITG_UART ))
&& (rbits & 1))
{
        icpi = mpc->mpc_icpi;
        icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
        cur_chnl_sync(mpc);
        rx_events &= ~CHAN_ATTN_SET;
        cur_chnl_sync(mpc);
        rx_events &= ~CHAN_ATTN_SET;
} else
					if(rbits & 1) {
						icpi = mpc->mpc_icpi;
						icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
						FREEZ_BANK(mpc);
					   if (!(Gicp_initiate & RNG_CLK_ON)) 
							mpc->mpc_cin_events &= ~EV_LMX_CNG;
					if ((!(icpb->ssp.bank_sigs & 0x80)) &&
					  (icp->lmx[mpc->mpc_lmxno].lmx_active != DEV_GOOD))
							mpc->mpc_cin_events &= ~EV_LMX_CNG;
					if (icpb->ssp.bank_sigs & 0x20)
							mpc->mpc_cin_events &= ~EV_LMX_CNG;
if(!(port % 16) &&
/*!(ISRAMP(mpc)) &&*/ (mpc->mpc_cin_events & EV_LMX_CNG) &&
                                                (icpi->ssp.cin_attn_ena &
                                                EV_LMX_CNG)){
							int lmx_flag = 1;
							mpc->mpc_cin_events &= 
						   	~EV_LMX_CNG;
							if (lmx_flag) {
#ifdef DEBUG
printk("ev_lmx_chg  rng_last=%x  port=%d\n",icp->icp_rng_last,port);
#endif
							if (!(icp->lmx[mpc->mpc_lmxno].
								lmx_type & 1)) {
								icpo = mpc->mpc_icpo;
								icpo->ssp.cout_lmx_cmd =
							   	0x40; 
								frame_wait( mpc, 6 );
								icpo->ssp.cout_lmx_cmd =
							   	0;
								mega_rng_delta(icp, 
								RNG_FAIL, 
								mpd->mpd_nchan);
							} else { /* not a panel */
								if((rx_ctrl_sig & 0xe0)
							   	== 0x80) { 
									/* bridge 
								 	* offline */
									/* ensure active
								 	* bank valid */
									frame_wait(mpc,
									2); 
									mega_rng_delta(
									icp, 
									RNG_FAIL, 
									mpd->mpd_nchan);
								} else {  
									if(icp->
									icp_rng_last & 
									RNG_FAIL)
										mega_rdv_delta(icp, mpc, RNG_BAD);
									else
										mega_rdv_delta(icp, mpc, RNG_GOOD);
								}  
							}	/* not a panel */
							}
					
						}
						else
							mpc->mpc_cin_events &= 
						   	~EV_LMX_CNG;
						if(mpc->mpc_cin_events & EV_REG_UPDT) {
							mpc->mpc_cin_events &= 
						   	~EV_REG_UPDT;
							rx_cie &= ~ENA_REG_UPDT;
						}
						if(mpc->mpc_cin_events & 
							(EV_BREAK_CNG)) {
							megasint(mpc, flags);
							mpc->mpc_cin_events &= 
						   	~EV_BREAK_CNG;
						}
						if(mpc->mpc_cin_events & EV_OVERRUN) {
							mpc->mpc_cin_events &= ~EV_OVERRUN;
						}
						if(mpc->mpc_cin_events & EV_VMIN) {
	     						megainput(mpc, flags);
							mpc->mpc_cin_events &= ~EV_VMIN;
						}
						if(mpc->mpc_cin_events & EV_VTIME) {
							mpc->mpc_cin_events &= 
						   	~EV_VTIME;
						}
						if(mpc->mpc_cin_events & EV_DCD_CNG) {
							megamint(mpc);
							mpc->mpc_cin_events &= 
						   	~EV_DCD_CNG;
						}
					}	/* rx event per port */
					if (!rbits && !xbits ) {
						if (port <= 31) {
							mpc += (31 - port);
							port = 31;
						}
						else
							break;
					}
					if(port == 31) {
						rbits = rbits1;
						xbits = xbits1;
						if (!rbits && !xbits )
							break;
					} else {
						rbits = rbits >> 1;
						xbits = xbits >> 1;
					}		
				}		/* ports loop */
			} else {

				/* SSP4 board */
				volatile union global_regs_u *icp_glo =
				   (volatile union global_regs_u *)((unsigned long)mpd->icp[j].icp_regs_start + 0x400);
				g_attn = icp_glo->ssp2.attn_pend;
				if(g_attn & 0x01 || icp->icp_poll_defer) { /* check global attn bit */
					icp->icp_poll_defer = 0;
        			icp_glo->ssp2.bus_cntrl &= ~0x10;/* allow clear when read */
        			rxtx_work = icp_glo->ssp2.chan_attn;

        			icp_glo->ssp2.bus_cntrl |= 0x10; /* block clear when read */
        
        			for ( ii = 0; ii < 4; ii++,mpc++ )  /* DEBUG: testing 4 chnls for 2 and 4 */
        			{

            		if ( rxtx_work & 0x01 )
            		{    /* TX WORK */
				icpo = mpc->mpc_icpo;
				icpq = &icpo->ssp.cout_q0;
				TX_EVENTS(mpc->mpc_cout_events, mpc);
				if(mpc->mpc_cout_events & EV_TX_EMPTY_Q0) {
#ifdef DEBUG1
	printk("EMPTY_Q0 event for device %d\n", 
	     		mpc->mpc_chan + (i * NCHAN_BRD) + (j * NCHAN));
#endif
					if (Q_data_count)
						tx_cie |= (ENA_TX_EMPTY_Q0);
					else{
						if (!mpc->xmit_cnt)
							mpc->mpc_flags 
								&= ~MPC_BUSY;
						megatxint(mpc);

						if (write_wakeup_deferred) {
							spin_unlock_irqrestore(&mpd->mpd_lock, flags);
							write_wakeup_deferred = 0;
							mpc->mpc_tty->ldisc.write_wakeup(mpc->mpc_tty);
							spin_lock_irqsave(&mpd->mpd_lock, flags);
						}
					}
					mpc->mpc_cout_events &= 
				   		~EV_TX_EMPTY_Q0;
				}
				else if(mpc->mpc_cout_events & EV_TX_LOW_Q0) {
#ifdef DEBUG
	printk("LOW_Q0 event for device %d\n", 
	     		mpc->mpc_chan + (i * NCHAN_BRD) + (j * NCHAN));
	icpq = &icpo->ssp.cout_q0;
	printk("Q data count for device %d\n", Q_data_count);
	printk("frame counter for device %d\n", icpo->ssp2.frame_ctr);
	printk("q block count %d\n", icpq->ssp.q_block_count);
#endif
					if (Q_data_count >=
						(icpq->ssp.q_block_count * 64))
						tx_cie |= (ENA_TX_LOW_Q0);
					else {
						megatxint(mpc);

						if (write_wakeup_deferred) {
							spin_unlock_irqrestore(&mpd->mpd_lock, flags);
							write_wakeup_deferred = 0;
							mpc->mpc_tty->ldisc.write_wakeup(mpc->mpc_tty);
							spin_lock_irqsave(&mpd->mpd_lock, flags);
						}
					}
					mpc->mpc_cout_events &= 
					   	~EV_TX_LOW_Q0;
				}
				if(mpc->mpc_cout_events & EV_OUT_TMR_EXP) {
	   				mpc->mpc_cout_events &= ~EV_OUT_TMR_EXP;
				}
				icpo->ssp.cout_attn_ena |= 0x8000;	
            		}  /* TX WORK */

            		rxtx_work = rxtx_work >> 1;   /* prepare next channel */

			if ( (rxtx_work & 0x08) || (mpc->mpc_flags & MPC_DEFER) ) { 
/* Defer processing to every 40 mills (<= 115Kb)*/
/* Defer processing to every 20 mills (== 230Kb)*/
if (mpc->mpc_tty != (struct tty_struct *) NULL){
  	unsigned int baud = mpc->mpc_tty->termios->c_cflag & CBAUD;
	if (baud == B38400){
		if ((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
			baud = B57600;
		else if ((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
			baud = B115200;
	}
	if ((baud < B230400) && (poll_cnt & 3)) {
		icp->icp_poll_defer = 1;
		mpc->mpc_flags |= MPC_DEFER; 
		continue;
	}
	if ((baud == B230400) && (poll_cnt & 1)) {
		icp->icp_poll_defer = 1;
		mpc->mpc_flags |= MPC_DEFER; 
		continue;
	}
}
else
	if (poll_cnt & 3) { 
		icp->icp_poll_defer = 1;
		mpc->mpc_flags |= MPC_DEFER; 
		continue;
	}
mpc->mpc_flags &= ~MPC_DEFER; 
	  				icpi = mpc->mpc_icpi;
               				FREEZ_BANK(mpc);
 	     				mpc->mpc_cin_events &= ~EV_LMX_CNG;
	  				if(mpc->mpc_cin_events & EV_REG_UPDT) {
	     					mpc->mpc_cin_events &= ~EV_REG_UPDT;
	     					rx_cie &= ~ENA_REG_UPDT;
	  				}
	  				if(mpc->mpc_cin_events & EV_VMIN) {
#ifdef DEBUG
				icpo = mpc->mpc_icpo;
	printk("frame counter on input with poll_cnt = %d, %d\n", poll_cnt, icpo->ssp2.frame_ctr);
#endif
	     					megainput(mpc, flags);
	     					mpc->mpc_cin_events &= ~EV_VMIN;
	  				}
	  				if(mpc->mpc_cin_events & EV_VTIME) {
	     					mpc->mpc_cin_events &= ~EV_VTIME;
	  				}
	  				if(mpc->mpc_cin_events & (EV_BREAK_CNG|EV_CHAR_LOOKUP)) {
	     					megasint(mpc, flags);
	     					mpc->mpc_cin_events &= ~EV_BREAK_CNG;
	  				}
	  				if(mpc->mpc_cin_events & EV_DCD_CNG) {
	     					megamint(mpc);
	     					mpc->mpc_cin_events &= ~EV_DCD_CNG;
	  				}
				icpi->ssp.cin_attn_ena |= 0x8000;	
            		}      /* RX WORK */
        			} /* for(ii = 0; ii < 4; ii++) */
				} /* Global attention bit set */
  			} /* SSP2/4 rx/tx work */
			if(win16)
	   			mega_pop_win(29);
		}		/* icps loop */

		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
	}			/* adapters loop */

	eqnx_timer.expires = jiffies + MPTIMEO;
	add_timer(&eqnx_timer);
}

/*
** mega_rng_delta(icp, reason, nchan)
**
**   Handle a change in the ring pointed to by "icp".  "reason" may be
**        
**     RNG_BAD         status just went to bad
**     RNG_GOOD        status just went to good (includes init time)
**     RNG_WAIT        debounce of RNG_CLK in progress
**     RNG_CHK         service of a transistion to RNG_GOOD in progress
**                                
**   Initialization code relies on this state machine to establish the
**   ring as well as ring failure detection and re-initialization.
**   This routine is responsible for "local" lmx's only!!!
**        
**   Return the new value for "rng_reqsvc" to indicate if this routine
**   should be run next entry of megapoll() without any change in the
**   ring status.
**
**   An inherent assumption in ring processing is that ring processing
**   has precedence over lmx processing.  This implies:
**     1) changes in local lmx's will be prompted by loss of ring.
**     2) lmx condition change events will be due to mux
**        changes (not local lmx changes)
** 
** mpdev board lock ** MUST ** be held			 
*/
static int mega_rng_delta( icpaddr_t icp, int reason, int nchan)
{
register icpbaddr_t icpb;
register icpiaddr_t icpi;
register icpoaddr_t icpo;
icpgaddr_t icpg;
int chnl, lx;
int frames_ok = 1;
struct mpchan *mpc;
struct lmx_struct *lmx;
struct slot_struct *slot_ptr;
int ldv_chng = false;
unsigned int addr;
unsigned int bit_test;

  switch ( reason )
  {
    case RNG_FAIL:
	icp->icp_rng_last = RNG_FAIL;

    case RNG_BAD:
	if(icp->icp_rng_state == RNG_BAD)
	   break;	
	if( !eqn_quiet_mode && !EQNft )

               printk("WARNING: SST expansion bus failure detected for minor devices %d to %d\n",
               icp->icp_minor_start, icp->icp_minor_start + MAXICPCHNL - 1 );
/* RAMP START -------------------------------------------------- */
      for(lx = 0; lx < 4; lx++)
      {
	  lmx = &icp->lmx[lx];
          if((lmx->lmx_id == 0x08) ||
               (lmx->lmx_id == 0x09) ||
               (lmx->lmx_id == 0x0B) )
          {
           int i, fret;
           mpc = lmx->lmx_mpc;

#ifdef	DEBUG_LOCKS
	   if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in mega_rng_delta()\n");
	   }
#endif	/* DEBUG_LOCKS */

	      if((fret = mpa_srvc_dereg_mpa(icp->sspcb,(unsigned char)(lx * 16),
                 &slot_ptr)))
	       {
                  MESSAGE("ERROR: dereg_mpa", fret, mpc);
	       }
	       else	/* success: place engines online */	
	       { 	
                  MESSAGE("SUCCESS: dereg_mpa", 
                     icp->sspcb->mpa[lx].mpa_num_engs, mpc);
	       }
            if (eqn_num_ramps > 0)
               eqn_num_ramps--;
            if (eqn_num_ramps == 0) {
               if ( to_eqn_ramp_admin != -1 ) {
		       if (timer_pending(&eqnx_ramp_timer))
				del_timer(&eqnx_ramp_timer);
	       }

               to_eqn_ramp_admin = -1;
            }
            for(i = 0; i < lmx->lmx_chan; i++, mpc++) {
               ramp_check_error(mpc, fret);
               ramp_dereg_modem_cleanup(mpc);
            }
          }
      }
/* RAMP END ---------------------------------------------------- */
      /* the icp should have flow controlled channel data */
      /* set state of ldev's and any rdev's to bad */
      icp->icp_rng_fail_count++;
      icp->icp_rng_state = RNG_BAD;
      icp->lmx[0].lmx_active = DEV_BAD;
      icp->lmx[1].lmx_active = DEV_BAD;
      icp->lmx[2].lmx_active = DEV_BAD;
      icp->lmx[3].lmx_active = DEV_BAD;
      /* allow LMX_CNG events in RNG_BAD state */
      /*icp->lmx[0].lmx_mpc->mpc_icpi->ssp.cin_attn_ena &= ~0x40;
      icp->lmx[1].lmx_mpc->mpc_icpi->ssp.cin_attn_ena &= ~0x40;
      icp->lmx[2].lmx_mpc->mpc_icpi->ssp.cin_attn_ena &= ~0x40;
      icp->lmx[3].lmx_mpc->mpc_icpi->ssp.cin_attn_ena &= ~0x40;*/
      icp->lmx[0].lmx_rmt_active = DEV_BAD;
      icp->lmx[1].lmx_rmt_active = DEV_BAD;
      icp->lmx[2].lmx_rmt_active = DEV_BAD;
      icp->lmx[3].lmx_rmt_active = DEV_BAD;
      break;

    case RNG_GOOD:
	if(icp->icp_rng_state == RNG_GOOD)  {
	  break;	
	}
      /* this occurs as a normal part of initialization */
      /* loop thru all possible lmx's - check chnls 0, 16, 24, 48 */
      /* hold processor here while local lmx's are init'd */
      /* setup appropriate state flags so that remote lmx configuration
        takes place during "lmx_cond_changed" processing */

	if(icp->icp_rng_good_count && !eqn_quiet_mode )
        /* no message on initial find */
                 printk("NOTICE: SST expansion bus established for minor devices %d to %d\n",
                 icp->icp_minor_start, icp->icp_minor_start + MAXICPCHNL - 1 );
	icp->icp_rng_state = RNG_CHK;
	lmx = &icp->lmx[0];
        mpc = lmx->lmx_mpc;
	icpo = mpc->mpc_icpo;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in mega_rng_delta()\n");
	   }
#endif	/* DEBUG_LOCKS */

	/* tested in "for" loop */
      frames_ok = frame_ctr_reliable( (icpgaddr_t ) icpo, mpc );  
      for ( chnl = 0, lx = 0; chnl < MIN(MAXICPCHNL, nchan); chnl += 16, lx++ )
      {
      int prod_id;
      int retv, ramp, ramp_id, lmx_speed;
      int ami_interface;
      int no_lmx;
      int mario; /* ATMEL FIX */

	lmx = &icp->lmx[lx];
        mpc = lmx->lmx_mpc;
	icpi = mpc->mpc_icpi;
	icpo = mpc->mpc_icpo;

          icpo->ssp.cout_cpu_req |= 0x80; /* Pause output */  
          frame_wait( mpc, 3 );
          icpo->ssp.cout_lmx_cmd = 0xc0; /* make sure we are offline */  
          frame_wait( mpc, 6 );
          icpo->ssp.cout_lmx_cmd = 0x40; /* make sure we are offline */  
          frame_wait( mpc, 6 );
          icpo->ssp.cout_lmx_cmd &= ~0x40;
          icpo->ssp.cout_cpu_req &= ~0x80; /* Resume output */  
          frame_wait( mpc, 3 );

	icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
	if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
		/* SSP64 */
		icpg = (icpgaddr_t) icpo;
	else
		/* SSP2 / SSP4 */
		icpg = (icpgaddr_t)((unsigned long)(mpc->mpc_icpi) + 0x400);
/* Fix ATMEL State Machine Problem 4.01 */
for (mario=0; mario < 16; mario++)
{
	bit_test = (0x5a5a ^ ((16*lx) + mario)) & ~0x3;
	(mpc + mario)->mpc_icpi->ssp.cin_locks |= LOCK_A;
	(mpc + mario)->mpc_icpi->ssp.cin_locks &= ~LOCK_B;
	cur_chnl_sync(mpc); /* mpc is sufficiently initialized */
	(mpc + mario)->mpc_icpi->ssp.cin_bank_a.ssp.bank_fifo_lvl &= ~0x8f;
	(mpc + mario)->mpc_icpi->ssp.cin_bank_a.ssp.bank_dma_ptr = bit_test;
	(mpc + mario)->mpc_icpi->ssp.cin_locks |= LOCK_B;
	(mpc + mario)->mpc_icpi->ssp.cin_locks &= ~LOCK_A;
	cur_chnl_sync(mpc); /* mpc is sufficiently initialized */
	(mpc + mario)->mpc_icpi->ssp.cin_bank_b.ssp.bank_fifo_lvl &= ~0x8f;
	(mpc + mario)->mpc_icpi->ssp.cin_bank_b.ssp.bank_dma_ptr = bit_test;
	(mpc + mario)->mpc_icpi->ssp.cin_tail_ptr_a = bit_test;
	(mpc + mario)->mpc_icpi->ssp.cin_tail_ptr_b = bit_test;
}
frame_wait( mpc, 1 );
for (mario=0; mario < 16; mario++)
{
   bit_test = (0x5a5a ^ ((16*lx) + mario)) & ~0x3;
   if (((mpc+mario)->mpc_icpi->ssp.cin_tail_ptr_a == bit_test) &&
       ((mpc+mario)->mpc_icpi->ssp.cin_tail_ptr_b == bit_test) &&
       ((mpc+mario)->mpc_icpi->ssp.cin_bank_a.ssp.bank_dma_ptr == bit_test) &&
       ((mpc+mario)->mpc_icpi->ssp.cin_bank_b.ssp.bank_dma_ptr == bit_test))
   {
       continue;
   }
   else
   {
      Gicp_initiate = 0x1c;
      frame_wait( mpc, 1 );
      Gicp_initiate = 0x1e;
      frame_wait( mpc, 1 );
      icp->icp_rng_last = RNG_FAIL;
      icp->icp_rng_state = RNG_BAD;
      return 0;
   }
}
/* End ATMEL fix part 1 */

/* Fix ATMEL State Machine Problem 4.02 */
for (mario=0; mario < 16; mario++)
{
   int addr;
   struct mpchan *mpc1;
   icpiaddr_t icpi;
   int j; /* 3.49 */
   unsigned char *ptr; /* 3.49 */
   mpc1 = mpc + mario;
   icpi = mpc1->mpc_icpi;

   Gicp_initiate = 0x1A; 
   frame_wait( mpc1, 1 );
   /* 3.49 */
   ptr = (unsigned char *) icpi;
   for (j=0x40; j<0x80; j++)
	ptr[j] = 0;
   /* 3.49 */
   icpi->ssp.cin_bank_a.ssp.bank_fifo_lvl &= ~0x8f;
   icpi->ssp.cin_bank_b.ssp.bank_fifo_lvl &= ~0x8f;
/**** New 3.49 ATMEL fix for both Flat and Paged Mode ****/
   addr = (mpc1->mpc_rxpg * 0x4000) + mpc1->mpc_rxq.q_begin;
   mpc1->mpc_rxq.q_ptr = mpc1->mpc_rxq.q_begin;
   icpi->ssp.cin_bank_a.ssp.bank_dma_ptr
      = icpi->ssp.cin_bank_b.ssp.bank_dma_ptr
      = icpi->ssp.cin_tail_ptr_a 
      = icpi->ssp.cin_tail_ptr_b 
      = addr & 0xffff;
/**** End New 3.49 ****/
   icpi->ssp.cin_inband_flow_ctrl = 0;  /* calibrate */
   cur_chnl_sync(mpc1);
   Gicp_initiate = 0x1e;
   frame_wait( mpc1, 1 );
   FREEZ_BANK(mpc1);
   FREEZ_BANK(mpc1);
   mpc1->mpc_cin_events = 0;
}
/* End ATMEL fix part 2 */

frame_wait( mpc, 1 );
if(i_gicp_attn & RNG_FAIL) {
   icp->icp_rng_last = RNG_FAIL;
   icp->icp_rng_state = RNG_BAD;
   return 0;
}
/* End ATMEL fix part 1 */

icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
	/* new 2.03a */
	if(rx_ctrl_sig & 0x20)  { /* Not offline, try again next megapoll */
		icp->icp_rng_last = RNG_FAIL;
		icp->icp_rng_state = RNG_BAD;
		break;
	}
	/* end 2.03a */
	if((rx_ctrl_sig & 0x80) && frames_ok ) {
          /* lmx present at this position */
	  /* GDC: Changed to and with 0x3f instead of 0x0f */
          prod_id = (unsigned)((mpc +1)->mpc_icpi->ssp.cin_tdm_early & 0x3f) >> 1;
/* RAMP START ---------------------------------------------- */
	  ramp_id =
           (unsigned)((mpc+1)->mpc_icpi->ssp.cin_tdm_early & 0xffff); 
	  ramp_id = ((ramp_id  >> 1) & 0x3f);

	  if(ramp_id == 0x08 || ramp_id == 0x09 || ramp_id == 0x0B)
	      ramp = true;
	  else
	      ramp = false;
	 
	  if(ramp)
	  {    /* clear in_use field */
	      int ii;
              for(ii = 0; ii < 16; ii++)
	         (icp->slot_ctrl_blk + chnl + ii)->in_use = 0x00; 

	         if((retv = mpa_srvc_reg_mpa(icp->sspcb, 
				  (unsigned char)chnl,
				  (icp->slot_ctrl_blk + chnl),
				  (struct mpa_input_s *)(mpc->mpc_icpi))) )
	          {

#ifdef DEBUG_RAMP
                printk("ERROR reg_mpa: ret = %d, chan = %d\n", 
			retv, mpc->mpc_chan);
#endif
               MESSAGE("ERROR reg_mpa:",retv,mpc);
	          }
	          else	/* success: place engines online */	
	          { 	
#ifdef DEBUG_RAMP
                printk("SUCCESS reg_mpa: ret = %d, chan = %d\n", 
			retv, mpc->mpc_chan);
#endif
                MESSAGE("SUCCESS reg_mpa:", retv, mpc);
  			if ( to_eqn_ramp_admin != -1 ) {
				if (timer_pending(&eqnx_ramp_timer))
					del_timer(&eqnx_ramp_timer);
			}
			to_eqn_ramp_admin = 1;
			
			/*
	 		* Set up the timer channel.  
	 		*/
			init_timer(&eqnx_ramp_timer);
			eqnx_ramp_timer.expires = jiffies + 210;
			eqnx_ramp_timer.data = 0;
			eqnx_ramp_timer.function = &eqn_ramp_admin;
			add_timer(&eqnx_ramp_timer);
                        retv = 0;
	          }
                  eqn_num_ramps++;
                  ramp_admin_poll = RAMP_FAST_TIME;
	  }		  	   
/* RAMP END ------------------------------------------------ */
          if(lmx->lmx_active == DEV_BAD && prod_id != lmx->lmx_id)
          {
            /* ring configuration has changed */
	    ldv_chng = true;
            mega_ldv_hangup( icp );
	    icp->icp_active = DEV_VIRGIN;
            lmx->lmx_id = prod_id;
            icpo->ssp.cout_lmx_cmd &= ~0x90;
          }
            
          /* clear/set scan speed */
          lmx_speed = (unsigned)(icpi->ssp.cin_tdm_early & 0xc0) >> 6;
	  if (!ramp) {
          Gicp_scan_spd &= ~(0x03 << (lx * 2));
          Gicp_scan_spd |= lmx_speed << (lx * 2);
	  }

          /* put local lmx online
               - set loopback in case external device is streaming data 
               - guarantees supervisory frames */
/* online, internal loopback, lock lmx, cmd enable */
		/* lock disabled 04/01/98 */
          icpo->ssp.cout_lmx_cmd = 0x49; 
          frame_wait( mpc, 6 );
          /* use bank_sigs - can't use cin_tdm_early here! */
          ami_interface = rx_ctrl_sig & 0x2000;
          no_lmx = !( rx_ctrl_sig & 0x20 );  /* lmx_online */
          /* clear cmd enable and loopback */
          icpo->ssp.cout_lmx_cmd &= ~0x41;
          if ( !no_lmx )
          {
            /* lmx went online as expected */
            lmx->lmx_speed = lmx_speed;
	    if(!ramp)
            lmx->lmx_id = prod_id;
	    else
		lmx->lmx_id = ramp_id;

/* if we are the secondary icp, tell the icp to say so */
            if ( rx_ctrl_sig & 0x200 )  
               Gicp_initiate |= 1;
            else
               Gicp_initiate &= ~1;
		
/* Fix ATMEL State Machine Problem 3.48 */
for (mario=0; mario < 16; mario++)
{
   struct mpchan *mpc1;
   icpiaddr_t icpi;
   int	j;		/* 4.02 */
   unsigned char *ptr;	/* 4.02 */
   mpc1 = mpc + mario;
   /* Restore Loopback if enabled */
   if ((mpc1->mpc_flags & MPC_LOOPBACK) && (!ramp)) {
      /* int/ext loopback, command enable */
      mpc1->mpc_cout->ssp.cout_lmx_cmd |= 0x43;
      frame_wait( mpc1, 6 );
      /* clear cmd enable */
      mpc1->mpc_cout->ssp.cout_lmx_cmd &= ~0x40;
   }
   icpi = mpc1->mpc_icpi;
   Gicp_initiate = 0x1A;
   frame_wait( mpc1, 1 );
   /* 4.02 */
   ptr = (unsigned char *) icpi;
   for (j=0x40; j<=0x80; j++)
	   ptr[j] = 0;
   /* 4.02 */
   
   icpi->ssp.cin_bank_a.ssp.bank_fifo_lvl &= ~0x8f;
   icpi->ssp.cin_bank_b.ssp.bank_fifo_lvl &= ~0x8f;

   addr = (mpc1->mpc_rxpg * 0x4000) + mpc1->mpc_rxq.q_begin;
   mpc1->mpc_rxq.q_ptr = mpc1->mpc_rxq.q_begin;
   icpi->ssp.cin_bank_a.ssp.bank_dma_ptr
         = icpi->ssp.cin_bank_b.ssp.bank_dma_ptr
         = icpi->ssp.cin_tail_ptr_a 
         = icpi->ssp.cin_tail_ptr_b 
         = (addr & 0xffff);

   icpi->ssp.cin_inband_flow_ctrl = 0;  /* calibrate */
   cur_chnl_sync(mpc1);
   Gicp_initiate = 0x1e;
   frame_wait( mpc1, 1 );
   FREEZ_BANK(mpc1);
   FREEZ_BANK(mpc1);
   mpc1->mpc_cin_events = 0;
}
/* End ATMEL fix part 2 */

            if ( ami_interface )
            {
              lmx->lmx_active = DEV_GOOD;
              lmx->lmx_type |= 1; /* indicate a bridge */
              /* set local lmx transparent */
              icpo->ssp.cout_lmx_cmd = 0x78;  /* online, transparent, lock lmx,
                                                               cmd enable */
              frame_wait( mpc, 6 );
              /* clear cmd enable */
              icpo->ssp.cout_lmx_cmd &= ~0x40;
		lmx->lmx_chan = 16; /* remotes are all 16 at this time */
	      lmx->lmx_rmt_active = DEV_VIRGIN;
              /* remainder of processing takes place under "lmx cond changed"
                 processing in mega_rdv_delta() */
                 
              /* unmask "lmx condition changed" so processing will continue */
              icpi->ssp.cin_attn_ena |= 0x40;
            }  /* ami_interface */
            else
            {
              /* no ami_interface - panel present */
              lmx->lmx_type &= ~1; /* indicate not a bridge */
              /* configure ldv record for panel */

	      switch ( lmx->lmx_id )
	      {
		case 3:  /* software selectable speed multiplier */
		  switch( mpc->mpc_mpd->mpd_id & 0xf8 )
		  {
		    case 0x18:
                      lmx->lmx_chan = 8;
		      break;
		    case 0x20:
                      lmx->lmx_chan = 4;
		      break;
		    default:  /* shouldn't happen */
                      lmx->lmx_chan = MAXLMXMPC;
		      break;
		  }
		  break;
		case 4:
                  lmx->lmx_chan = 8;
		  break;
		case 5:
                  lmx->lmx_chan = 12;
		  break;
		case 6:
                  lmx->lmx_chan = 8;
		  break;
		case 7:
                  lmx->lmx_chan = 16;
		  break;
		case 8:		/* RAMP found */
		  lmx->lmx_chan = 16;
		  break;
		case 9:
		  lmx->lmx_chan = 8;
		  break;
		case 0xB:
		  lmx->lmx_chan = 4;
		  break;
/* GDC */
		case 0x10:  /* GDC */
		  lmx->lmx_chan = 16;
		  break;
		default:
                  lmx->lmx_chan = MAXLMXMPC;
		  break;
	      }

	      if ( (lmx->lmx_chan == 8 
	           || lmx->lmx_chan == 4)
                   && (!ramp ))
	      {
		unsigned char cin_lck[16];
		unsigned char cout_lck[16];
		volatile unsigned char *chan_ptr = &(Gicp_chnl),cur_chan;
		int i=0;
                  int kk = 0;
                  /* change lmx speed for this device */
                  while (true) {
                    (icpo + kk)->ssp.cout_ctrl_sigs |= 0x100;
                    (icpo + kk + 1)->ssp.cout_ctrl_sigs &= ~0x100;
                    kk += 2;
                    if(kk >= 16) break;
                  }
                  frame_wait(mpc, 6);
		for (kk=0; kk<16; kk++) {
		  cin_lck[kk] = (mpc + kk)->mpc_icpi->ssp.cin_locks;
		  (mpc + kk)->mpc_icpi->ssp.cin_locks = 0xff;
		  cout_lck[kk] = (mpc + kk)->mpc_icpo->ssp.cout_lock_ctrl;
		  (mpc + kk)->mpc_icpo->ssp.cout_lock_ctrl = 0xff;
		}
		cur_chan = *chan_ptr;
		while(*chan_ptr == cur_chan) { if( ++i > 9000) break; }
                Gicp_scan_spd &= ~(0x03 << (lx * 2));
                Gicp_scan_spd |= (1 << (lx*2));
                lmx->lmx_speed = 1;
		i=0;
		cur_chan = *chan_ptr;
		while(*chan_ptr == cur_chan) { if( ++i > 9000) break; }
		for (kk=0; kk<16; kk++) {
			(mpc + kk)->mpc_icpi->ssp.cin_locks = cin_lck[kk];
			(mpc + kk)->mpc_icpo->ssp.cout_lock_ctrl = cout_lck[kk];
		}
	      }

              /* set alive */
              lmx->lmx_active = DEV_GOOD;
              /* panel ready for use */
              if ( icp->icp_rng_good_count && !eqn_quiet_mode )
	      /* no message on initial find */
                         printk("NOTICE: SST: minor devices %d to %d available\n",
                         icp->icp_minor_start + lx * MAXLMXMPC, 
                         icp->icp_minor_start + (lx + 1) * MAXLMXMPC - 1 );
	      FREEZ_BANK(mpc); /* flush lmx events */
 	      mpc->mpc_cin_events = 0;
#ifdef DEBUG_RAMP
		printk("setting lmx change attention for channel %d\n", mpc->mpc_chan);
#endif
              icpi->ssp.cin_attn_ena |= 0x40;
            }
          }  /* local lmx online as expected */
          else
          {
            /* local lmx not put online as expected */
#ifdef DEBUG_RAMP
		printk("lmx for channel %d did not go online\n", mpc->mpc_chan);
#endif
            icpo->ssp.cout_lmx_cmd = 0;
            lmx->lmx_id = -1;
            if(lmx->lmx_active == DEV_BAD)
            {
              mega_ldv_hangup( icp );
              lmx->lmx_active = DEV_VIRGIN;
              lmx->lmx_id = -1;
            }
/* ATMEL */
		icp->icp_rng_last = RNG_FAIL;
		icp->icp_rng_state = RNG_BAD;
		return 0;
/* ATMEL */
          }  /* lmx not online as expected */
        }  /* local lmx present 
             and inbound early/late tdm's match 
             and frames_ok */
        else
        {
#ifdef DEBUG_RAMP
               printk("mega_rng_delta: no lmx present at this position %d\n", lx);
#endif
          /* no local lmx present
            or inbound early/late tdm's do not match 
            or !frames ok */
          if(lmx->lmx_active == DEV_BAD )
            mega_ldv_hangup( icp ); 
          lmx->lmx_active = DEV_VIRGIN;
          lmx->lmx_id = -1;
        }
      }  /* for loop - case RNG_GOOD */
	icp->icp_rng_good_count++;
      break;

      /* ATMEL - 4.02 */
      case RNG_CHK:
          {
	  int mario; /* ATMEL FIX */
	  int badptr = 0;

	  if (icp->icp_rng_wait++ > 20) { /* Do this for 800 milliseconds */
		icp->icp_rng_state = RNG_GOOD;
		icp->icp_rng_wait = 0; 
	  }

	  lmx = &icp->lmx[0];
	  mpc = lmx->lmx_mpc;
	  icpo = mpc->mpc_icpo;

#ifdef	DEBUG_LOCKS
	  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in mega_rng_delta()\n");
	  }
#endif	/* DEBUG_LOCKS */

	  for (mario=0; mario < 64; mario++) {
		int baddr,eaddr;
		struct mpchan *mpc1;
		icpiaddr_t icpi;
		mpc1 = mpc + mario;
		if (mpc1->mpc_icp->lmx[mpc1->mpc_lmxno].lmx_active != DEV_GOOD)
			continue;
		icpi = mpc1->mpc_icpi;
		baddr = (mpc1->mpc_rxpg * 0x4000) + mpc1->mpc_rxq.q_begin;
		eaddr = (mpc1->mpc_rxpg * 0x4000) + mpc1->mpc_rxq.q_end;
		if ((icpi->ssp.cin_bank_a.ssp.bank_dma_ptr < (baddr & 0xffff)) ||
		    (icpi->ssp.cin_bank_a.ssp.bank_dma_ptr > (eaddr & 0xffff))) {
			badptr = 1;
			break;
		}
		if ((icpi->ssp.cin_bank_b.ssp.bank_dma_ptr < (baddr & 0xffff)) ||
		    (icpi->ssp.cin_bank_b.ssp.bank_dma_ptr > (eaddr & 0xffff))) {
			badptr = 1;
			break;
		}
		if ((icpi->ssp.cin_tail_ptr_a < (baddr & 0xffff)) ||
		    (icpi->ssp.cin_tail_ptr_a > (eaddr & 0xffff))) {
			badptr = 1;
			break;
		}
		if ((icpi->ssp.cin_tail_ptr_b < (baddr & 0xffff)) ||
		    (icpi->ssp.cin_tail_ptr_b > (eaddr & 0xffff))) {
			badptr = 1;
			break;
		}
	  }

	  if (badptr == 1) { /* Start Over */
	  	if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
			/* SSP64 */
			icpg = (icpgaddr_t) icpo;
	  	else
			/* SSP2 / SSP4 */
			icpg = (icpgaddr_t)((unsigned long)(mpc->mpc_icpi) + 
					0x400);
		Gicp_initiate = 0x1c;
		frame_wait( mpc, 1 );
		Gicp_initiate = 0x1e;
		frame_wait( mpc, 1 );
		icp->icp_rng_last = RNG_FAIL;
		icp->icp_rng_state = RNG_BAD;
		return 0;
	  }
	  }
	break;
	/* ATMEL - 4.02 */
      
    default:
      break;
  }  /* switch reason */
  return 0;
}  /* eqn_ring_delta */

/*
** mega_rdv_wait(arg)
**
*/
static void mega_rdv_wait( unsigned long arg)
{
	struct mpchan *mpc = (struct mpchan *) arg;
	register icpiaddr_t icpi = mpc->mpc_icpi;
	register icpaddr_t icp = mpc->mpc_icp;
	unsigned long flags;

	icp->lmx[mpc->mpc_lmxno].lmx_wait = -1;
	mpc->mpc_cin_events |= EV_LMX_CNG;
	if(mpc->mpc_mpd->mpd_nwin) {

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	   	mega_push_win(mpc, 0);
		rx_cie |= 0xc000;
	   	mega_pop_win(30);
		spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
	} else {
        	rx_cie |= 0xc000;
	}
}

/*
** mega_rdv_delta(icp, mpc, rng_state)
**
** Handle a change in the remote lmx pointed to by channel "mp".
** This change was indicated by "lmx condition changed" bit
** in input events register, now stored in "mpc->mpc_rx_bank".  
**
** Only channels 0, 16, 32 and 48 arrive here!
**
** Remote lmx init/failure/recovery is handled by this routine.
** Local lmx (ldev) init/failure/recovery is handled by 
** "mega_ring_delta" by detecting changes in the ring.  
**        
** The channel lock for "mp" is assumed held on entry.
**
** mpdev board lock ** MUST ** be held			 
*/
static void mega_rdv_delta( icpaddr_t icp, struct mpchan *mpc, int rng_state)
{
register icpiaddr_t icpi = mpc->mpc_icpi;
register icpoaddr_t icpo = mpc->mpc_icpo;
icpgaddr_t icpg = (icpgaddr_t)icpo;
register icpbaddr_t icpb;
struct lmx_struct *lmx;
int rmt_lmx_found, lmx_online;
int chnl;

#ifdef	DEBUG_LOCKS
  if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
	printk("LOCK Failure: mpd board lock NOT locked in mega_rdv_delta()\n");
  }
#endif	/* DEBUG_LOCKS */

  if (mpc->mpc_mpd->mpd_board_def->asic != SSP64) /* SSP2 / SSP4 only */
	icpg = (icpgaddr_t)((unsigned long)(mpc->mpc_icpi) + 0x400);

  if ( !(rx_tdm_early & 0x100)) 
       return;  /* not a bridge  */

  chnl = mpc->mpc_chan;
  lmx = &icp->lmx[mpc->mpc_lmxno];
  /* make sure the current bank is valid */
  	frame_wait(mpc,2); /* make sure regs are valid */ 
  icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;

  if((rx_ctrl_sig & 0x60) == 0x00) {	/* bridge not on line */
      icp->icp_rng_svcreq = RNG_FAIL;
      return;
  }

  switch ( lmx->lmx_rmt_active )  /* current state */
  {
    case DEV_VIRGIN:
    case DEV_BAD:
dev_bad:
      /* "tdm's from mux" should be on in "bank_sigs" if remote present */
      rmt_lmx_found = ((rx_tdm_early & 0x300) == 0x300); 
                                            /* tdm's from mux */
      if ( rng_state == RNG_GOOD )
	if ( rmt_lmx_found )
	{
          lmx->lmx_active = DEV_GOOD;
	  /* wait for link to settle down */
          lmx->lmx_rmt_active = DEV_WAITING;
          if ( lmx->lmx_wait == -1 ){
		init_timer(&lmx_wait_timer);
		lmx_wait_timer.expires = 100;
		lmx_wait_timer.data = (unsigned long) mpc;
		lmx_wait_timer.function = &mega_rdv_wait;
		add_timer(&lmx_wait_timer);
	  }
	}  
      break;

    case DEV_WAITING:
      if ( lmx->lmx_wait != -1 )
      {
	/* event occured while waiting */
	del_timer(&lmx_wait_timer);
	lmx->lmx_wait = -1;
	lmx->lmx_rmt_active = DEV_BAD;
	rmt_lmx_found = ((rx_tdm_early & 0x300) == 0x300); 
	lmx_online = rx_ctrl_sig & 0x20; 
	if (lmx_online && !rmt_lmx_found) 
		goto dev_bad;
	else
		icp->icp_rng_svcreq = RNG_FAIL;
      break;
      }
      /* wait for mux link to settle is complete */
      /* get mux id from odd channel */
        lmx->lmx_rmt_id = 
	     (unsigned)((mpc +1)->mpc_icpi->ssp.cin_tdm_early & 0x1fe) >> 1;
	lmx->lmx_rmt_type = 0;
      /* put remote lmx online */
      icpo->ssp.cout_lmx_cmd = 0xe8;  /* target mux, lock lmx,
                                                       lmx online, 
                                                       cmd enable */
      frame_wait( mpc, 6 );
      /* clear cmd enable */
      icpo->ssp.cout_lmx_cmd &= ~0x40;

      /* did it work? */
      rmt_lmx_found = rx_ctrl_sig & 0x60;  /* tdm from mux/lmx_online */
      if ( rmt_lmx_found != 0x60 )
        rmt_lmx_found = false;
      if ( rmt_lmx_found )
      {
        lmx->lmx_rmt_active = DEV_GOOD;
        /* no equalization - mux ready for use */

	/* Restore Loopback if enabled */
	if (mpc->mpc_flags & MPC_LOOPBACK) {
		/* int/ext loopback, command enable */
		mpc->mpc_cout->ssp.cout_lmx_cmd |= 0x43;
		frame_wait( mpc, 6 );
		/* clear cmd enable */
		mpc->mpc_cout->ssp.cout_lmx_cmd &= ~0x40;
	}

        if ( lmx->lmx_good_count++ && !eqn_quiet_mode ) /* initial find? */
                   printk("NOTICE: SST: minor devices %d to %d available on remote mux\n",
                   icp->icp_minor_start + chnl, 
                   icp->icp_minor_start + chnl + MAXLMXMPC - 1 );
	  FREEZ_BANK(mpc); /* flush lmx events */
 	  mpc->mpc_cin_events = 0;
          icpi->ssp.cin_attn_ena |= 0x40;
      }
      else
      {
	/* wait again for link to return */
        lmx->lmx_rmt_active = DEV_BAD;
	goto dev_bad;
      }
      break;
      
    case DEV_GOOD:
      rmt_lmx_found = rx_ctrl_sig & 0x40;
      lmx_online = rx_ctrl_sig & 0x20;
                                            /* tdm's from mux and lmx online */
      if( rng_state == DEV_GOOD )
      {
	if((!rmt_lmx_found && lmx_online) || (rmt_lmx_found && !lmx_online))
	{
	  /* remote lmx has disappeared */
	  /* wait for RNG_GOOD and rmt_lmx_found */
          lmx->lmx_rmt_active = DEV_BAD;
	       if( !eqn_quiet_mode )
                printk("WARNING: SST: remote mux failure detected for minor devices %d to %d\n",
                 icp->icp_minor_start + chnl, 
                 icp->icp_minor_start + chnl + MAXLMXMPC - 1 );
	  /* tdm's from mux and lmx offline - no second event will occur */
	  if(rmt_lmx_found && !lmx_online)
	     goto dev_bad;
	}
        else 
	{
	  /* ring failed, tear down */
	  icp->icp_rng_svcreq = RNG_FAIL;
          icpo->ssp.cout_lmx_cmd = 0xc0;
          frame_wait( mpc, 6 );
          icpo->ssp.cout_lmx_cmd = 0;
          lmx->lmx_active = DEV_BAD;
          icpo->ssp.cout_lmx_cmd = 0x40;
          frame_wait( mpc, 6 );
          icpo->ssp.cout_lmx_cmd = 0;
	}
      }
      else
      {
	/* ring is now bad - wait for ring processing */
      }
      break;

    default:
      /* ignore delta -
         wait for ring processing to set DEV_BAD or DEV_VIRGIN */
      break;
  }
}  /* mega_rdv_delta */


/*
** mega_ldv_hangup(icp)
**
*/
static void mega_ldv_hangup( icpaddr_t icp)
{
	return;
}  /* mega_ldv_hangup */

/*
** megatxint(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
static void megatxint(struct mpchan *mpc)
{
	struct tty_struct *tty = mpc->mpc_tty;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in megatxint()\n");
	}
#endif	/* DEBUG_LOCKS */

	if (tty == (struct tty_struct *) NULL)
		return;
#ifdef DEBUG
	printk("megatxint: device = %d\n", SSTMINOR(mpc->mpc_major, 
				mpc->mpc_minor));
#endif
	if (mpc->mpc_mpd->mpd_board_def->asic != SSP64)	/* SSP2 / SSP4 only */
		if (mpc->xmit_cnt){
			sst_write1(mpc, EQNX_TXINT);
			return;
		}

#ifdef DEBUG
	printk("megatxint: calling write_wakeup for device = %d\n", 
		SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) 
			&& tty->ldisc.write_wakeup)
			(tty->ldisc.write_wakeup)(tty);
		wake_up_interruptible(&tty->write_wait);
}

/*
** megainput(mpc, flags)
**
** Low speed input routine.
**
** mpdev board lock ** MUST ** be held			 
** NOTE - lock is dropped and then re-locked to do input 
*/
static void megainput( register struct mpchan *mpc, unsigned long flags)
{
	register struct tty_struct  *tp = mpc->mpc_tty;
	register icpiaddr_t icpi = mpc->mpc_icpi;
	register caddr_t bp;
	uchar_t curtags,tags_l,tags_h;
	caddr_t tgp;
	icpbaddr_t icpb = mpc->mpc_icpb;
	int win16 = mpc->mpc_mpd->mpd_nwin;
	unsigned int rcv_cnt;
	int fifo_cnt, i;
	ushort dma_ptr;
	unsigned int tagp;
	char fifo[0x10];
	unsigned short err;
	struct termios *tiosp;
	icpoaddr_t icpo;
	unsigned char *cbuf;
	char *fbuf;
	int ldisc_count = 0;
#ifdef MIDNIGHT
        unsigned char *src_addr;
        int align, bytes;
#endif

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in megainput()\n");
	}
#endif	/* DEBUG_LOCKS */

	if (mpc->mpc_tty == (struct tty_struct *) NULL)
		return;
	icpo = mpc->mpc_icpo;
#ifdef DEBUG
	printk("megainput: device %d with flip_count = %d\n", 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor),
			tp->flip.count);
#endif
	tiosp = tp->termios;
	mpc->mpc_flags &= ~MPC_RXFLUSH;

	/*
	 * Just return if input queue is in danger of overflowing and
	 * an input delimiter has been received, or lack of system
	 * resources prevent us from copying data from board to clists.
	 */
	if (mpc->mpc_block){
#ifdef DEBUG
	printk("megainput: device %d with flip_count = %d blocked\n", 
		SSTMINOR(mpc->mpc_major, mpc->mpc_minor), tp->flip.count);
#endif
		return;
	}
	if((mpc->mpc_flags & MPC_OPEN) == 0){
		return;
	}

	rcv_cnt = (rx_next + (rx_fifo_cnt & 0xf) - mpc->mpc_rxq.q_ptr) 
			& (mpc->mpc_rxq.q_size -1);

#ifdef DEBUG
	printk("megainput: device %d with flip_count = %d rcv_cnt = %d\n", 
		SSTMINOR(mpc->mpc_major, mpc->mpc_minor), 
		tp->flip.count, rcv_cnt);
#endif
	rcv_cnt = MIN(rcv_cnt, (2 * TTY_FLIPBUF_SIZE));

#if	(LINUX_VERSION_CODE >= 132624)
	/* 2.6.16+ kernels */
	rcv_cnt = MIN(rcv_cnt, tp->receive_room);
#else
	rcv_cnt = MIN(rcv_cnt, tp->ldisc.receive_room(tp));
#endif

	if(rcv_cnt == 0) {
#ifdef	DEBUG
	printk("megainput: device %d with flip_count = %d rcv_cnt = %d return\n", 
		SSTMINOR(mpc->mpc_major, mpc->mpc_minor), 
		tp->flip.count, rcv_cnt);
#endif
	   return;
	}

#if	(LINUX_VERSION_CODE >= 132624)
	/* 2.6.16+ kernels */
	rcv_cnt = tty_prepare_flip_string_flags(tp, &cbuf, &fbuf, rcv_cnt);
	if (rcv_cnt <= 0)
		return;
#else
	cbuf = tp->flip.char_buf;
	fbuf = tp->flip.flag_buf;
#endif
	/* copy fifo's to memory */
	fifo_cnt = rx_fifo_cnt & 0xf; 
	if(fifo_cnt) {
		for(i=0; i< fifo_cnt; i++)
			fifo[i] = rx_fifo[i];
	}
	dma_ptr = rx_next;

	/* check if there was an error char */
	if ((err = (mpc->mpc_cin_events & EV_PAR_ERR))) {
		if (tiosp->c_iflag & IGNPAR) {  
			err=0;
		}
		if (!(tiosp->c_iflag & INPCK) )
			err=0;
	}
	err |= mpc->mpc_cin_events & EV_FRM_ERR; 
	mpc->mpc_cin_events &= ~(EV_PAR_ERR | EV_FRM_ERR);

	/* Only need to copy tags when there is an error */ 
	if(err && fifo_cnt) {
  		uchar_t *tagptr;
		if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
			/* SSP64 */
  			tagptr = (uchar_t *) 
				mpc->mpc_mpd->icp[0].icp_tags_start;
		else
			/* SSP2 / SSP4 */
  			tagptr = (uchar_t *) 
			mpc->mpc_mpd->icp[mpc->mpc_icpno].icp_tags_start;
		tags_l = icpb->ssp.bank_tags_l;
		tags_h = icpb->ssp.bank_tags_h;
		if(win16) { 
			mega_push_win(mpc, 3);
			dma_ptr &= 0x3fff;
		}
		if (mpc->mpc_mpd->mpd_board_def->asic == SSP64) {
			/* SSP64 */
			tagp = rx_base << 16;
			tagp |= dma_ptr;
		}
		else
			/* SSP2 / SSP4 */
			tagp = dma_ptr;
		if(fifo_cnt > 4) {
			int h_tagp = tagp + 4;
			if(h_tagp > (ushort)(mpc->mpc_rxq.q_end -1))
				h_tagp = (ushort)mpc->mpc_rxq.q_begin;
			tagptr[h_tagp >> 2] &= ~fifo_mask[fifo_cnt];
			tagptr[h_tagp >> 2] |= tags_h & fifo_mask[fifo_cnt];
			tagptr[tagp >> 2] &= ~fifo_mask[4];
			tagptr[tagp >> 2] |= tags_l & fifo_mask[4];
		} else {
			tagptr[tagp >> 2] &= ~fifo_mask[fifo_cnt];
			tagptr[tagp >> 2] |= tags_l & fifo_mask[fifo_cnt];
		}
		if(win16) 
			mega_pop_win(53);
	}
	if(win16) {
	   mega_push_win(mpc, 1);
	   dma_ptr &= 0x3fff;
	}
	if(fifo_cnt) { 
		for(i=0; i< fifo_cnt; i++) {
			mpc->mpc_rxq.q_addr[dma_ptr] = fifo[i];
			if(dma_ptr++ > (ushort)(mpc->mpc_rxq.q_end -1)) 
				dma_ptr = (ushort)mpc->mpc_rxq.q_begin;
		}
	}	


	bp = mpc->mpc_rxq.q_addr + mpc->mpc_rxq.q_ptr;
	while (rcv_cnt) {
		int val_cnt = 0;
		int code = 0;
		int qptrCnt = mpc->mpc_rxq.q_ptr;
		/* If error, look for first invalid character based on tag bits */
		if (err) {
  			uchar_t *tagptr;
			if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
				/* SSP64 */
  				tagptr = (uchar_t *) 
					mpc->mpc_mpd->icp[0].icp_tags_start;
			else
				/* SSP2 / SSP4 */
  				tagptr = (uchar_t *) 
				mpc->mpc_mpd->icp[mpc->mpc_icpno].icp_tags_start;
			if(win16)  
				mega_push_win(mpc, 3);
			if (mpc->mpc_mpd->mpd_board_def->asic == SSP64) {
				/* SSP64 */
				tagp = rx_base << 16;
				tagp |= mpc->mpc_rxq.q_ptr;
			}
			else
				/* SSP2 / SSP4 */
				tagp = mpc->mpc_rxq.q_ptr;
			while (val_cnt < rcv_cnt) {
				tgp = tagptr + (tagp >> 2);
				curtags = *tgp >> ((tagp % 4) * 2);
				if ((curtags & 3) == 1) { 
					code = 1;
					break;
				}
				else if ((curtags & 3) == 2) {
					code = 2;
					break;
				}
				else{
					code = 0;
				}
				if(qptrCnt++ > (ushort)(mpc->mpc_rxq.q_end -1)){
               				qptrCnt = (ushort)mpc->mpc_rxq.q_begin;
					if (mpc->mpc_mpd->mpd_board_def->asic
						== SSP64) {
						/* SSP64 */
						tagp = rx_base << 16;
						tagp |= (ushort)mpc->mpc_rxq.q_begin;
					}
					else
						/* SSP2 / SSP4 */
						tagp = 
						(ushort)mpc->mpc_rxq.q_begin;
				}
				else
					tagp++;
				val_cnt++;
			}
			if(win16) 
				mega_pop_win(32);
			rcv_cnt -= val_cnt;
			/* Move as much data as the ld will take while 
				checking for wrap */
			while (val_cnt){
				int count;
				count = MIN(val_cnt, mpc->mpc_rxq.q_end - 
					mpc->mpc_rxq.q_ptr + 1);
#ifdef MIDNIGHT
                                src_addr = mpc->mpc_rxq.q_addr + 
					mpc->mpc_rxq.q_ptr;
                                align = ((unsigned long)src_addr) % 4;
                                if (align) {
                                  bytes = MIN(count,(4-align));
                                  memcpy(cbuf, src_addr, bytes);
                                }
                                else {
                                  bytes = 0;
                                }

                                if (count > bytes) {
                                  memcpy(cbuf+bytes, (mpc->mpc_rxq.q_addr + 
                                     mpc->mpc_rxq.q_ptr + bytes), count-bytes);
                                }
#else
				memcpy(cbuf, 
					(mpc->mpc_rxq.q_addr + 
					mpc->mpc_rxq.q_ptr), count);
#endif
				if ((mpc->mpc_flags & MPC_DSCOPER) 
					&& (dscope[0].chan == mpc - meg_chan)) {
					int d=0;
					unsigned char *ubase = 
						(mpc->mpc_rxq.q_addr 
							+ mpc->mpc_rxq.q_ptr);
					int room,move_cnt;
					room = DSQSIZE - 
					((dscope[d].next - dscope[d].q.q_ptr) 
						& (DSQMASK));
					if (count > room)
						dscope[d].status |= DSQWRAP;
                			else
                        			room = count;
					while (room > 0) {
					move_cnt = MIN(room,
					dscope[d].q.q_end - dscope[d].next + 1);
					memcpy(dscope[d].q.q_addr + 
						dscope[d].next,ubase, move_cnt);
                        		dscope[d].next += move_cnt;
                        		if (dscope[d].next > dscope[d].q.q_end)
                                		dscope[d].next = 
							dscope[d].q.q_begin;
                                	ubase += move_cnt;
                                	room -= move_cnt;
					} /* while room */ 
				dscope[d].scope_wait_wait = 0;
				wake_up_interruptible(&dscope[d].scope_wait);
				} /* if MPC_DSCOPE */

				val_cnt -= count;
				memset(fbuf, 0, count);
				cbuf += count;
				fbuf += count;
				ldisc_count += count;
				mpc->mpc_input += count;
				mpc->mpc_rxq.q_ptr += count;
				if (mpc->mpc_rxq.q_ptr > mpc->mpc_rxq.q_end)
					mpc->mpc_rxq.q_ptr 
						= mpc->mpc_rxq.q_begin;
			}
			if (code){
				unsigned char c = 
				*(mpc->mpc_rxq.q_addr + mpc->mpc_rxq.q_ptr);
				/* c is an error character */
				if (code == 1){
					ldisc_count++;
					*fbuf++ = TTY_PARITY;
					*cbuf++ = c;
					mpc->mpc_parity_err_cnt++;
				}
				else if (code == 2){
					ldisc_count++;
					*fbuf++ = TTY_FRAME;
					*cbuf++ = c;
					mpc->mpc_framing_err_cnt++;
				}
				else{
					ldisc_count++;
					*fbuf++ = TTY_NORMAL;
					*cbuf++ = c;
				}
				rcv_cnt--;
				mpc->mpc_input++;
				if (++mpc->mpc_rxq.q_ptr > mpc->mpc_rxq.q_end) {
					mpc->mpc_rxq.q_ptr = mpc->mpc_rxq.q_begin;
					bp = mpc->mpc_rxq.q_addr + mpc->mpc_rxq.q_ptr;
				}
				if ((mpc->mpc_flags & MPC_DSCOPER) 
					&& (dscope[0].chan == mpc - meg_chan)) {
	
					int d=0;
					dscope[d].buffer[dscope[d].next] = c;
					if (dscope[d].next == dscope[d].q.q_ptr) {
						dscope[d].scope_wait_wait = 0;
				  		wake_up_interruptible(&dscope[d].scope_wait);
					}
					if (dscope[d].next++ == dscope[d].q.q_ptr) 
						dscope[d].status |= DSQWRAP;
					if (dscope[d].next > dscope[d].q.q_end)
						dscope[d].next = dscope[d].q.q_begin;
				} /* if MPC_DSCOPE */
			}
			else if (rcv_cnt)
			{
				printk("Unknown error code value %d", code);
				break; /* Perhaps unnecessary... */
			}
		} 
		else {
			val_cnt = rcv_cnt;
			rcv_cnt = 0;
			while (val_cnt){
				int count;
				count = MIN(val_cnt, mpc->mpc_rxq.q_end - 
					mpc->mpc_rxq.q_ptr + 1);
#ifdef MIDNIGHT
                                src_addr = mpc->mpc_rxq.q_addr + 
					mpc->mpc_rxq.q_ptr;
                                align = ((unsigned long)src_addr) % 4;
                                if (align) {
                                  bytes = MIN(count,(4-align));
                                  memcpy(cbuf, src_addr, bytes);
                                }
                                else {
                                  bytes = 0;
                                }

                                if (count > bytes) {
                                  memcpy(cbuf+bytes, (mpc->mpc_rxq.q_addr + 
                                     mpc->mpc_rxq.q_ptr + bytes), count-bytes);
                                }
#else
				memcpy(cbuf, 
					(mpc->mpc_rxq.q_addr + 
					mpc->mpc_rxq.q_ptr), count);
#endif
				if ((mpc->mpc_flags & MPC_DSCOPER) 
					&& (dscope[0].chan == mpc - meg_chan)) {
					int d=0;
					unsigned char *ubase = 
						(mpc->mpc_rxq.q_addr 
							+ mpc->mpc_rxq.q_ptr);
					int room,move_cnt;
					room = DSQSIZE - 
					((dscope[d].next - dscope[d].q.q_ptr) 
						& (DSQMASK));
					if (count > room)
						dscope[d].status |= DSQWRAP;
                			else
                        			room = count;
					while (room > 0) {
					move_cnt = MIN(room,
					dscope[d].q.q_end - dscope[d].next + 1);
					memcpy(dscope[d].q.q_addr + 
						dscope[d].next,ubase, move_cnt);
                        		dscope[d].next += move_cnt;
                        		if (dscope[d].next > dscope[d].q.q_end)
                                		dscope[d].next = 
							dscope[d].q.q_begin;
                                	ubase += move_cnt;
                                	room -= move_cnt;
					} /* while room */ 
				dscope[d].scope_wait_wait = 0;
				wake_up_interruptible(&dscope[d].scope_wait);
				} /* if MPC_DSCOPE */

				val_cnt -= count;
				memset(fbuf, 0, count);
				cbuf += count;
				fbuf += count;
				ldisc_count += count;
				mpc->mpc_input += count;
				mpc->mpc_rxq.q_ptr += count;
				if (mpc->mpc_rxq.q_ptr > mpc->mpc_rxq.q_end)
					mpc->mpc_rxq.q_ptr 
						= mpc->mpc_rxq.q_begin;
			}
		} /* end  if err */
	}      /*  while rcv_cnt */
	if(win16) 
	   mega_push_win(mpc, 0);
	rx_tail(mpc->mpc_rxbase + mpc->mpc_rxq.q_ptr);
	if(win16) {
		mega_pop_win(33);
		mega_pop_win(34);
	}
#ifdef DEBUG
	printk("calling tty_schedule_flip with %d charsfor device %d\n",
		tp->flip.count, SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
#ifdef DEBUG
	printk("calling n_tty_receive_buf with %d chars for device %d\n",
		ldisc_count, SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif

#if	(LINUX_VERSION_CODE >= 132624)
	/* 2.6.16+ kernels */
	tty_flip_buffer_push(tp);
#else

	/*
	** drop mpdev board lock, reacquire after receive_buf
	*/
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
	tp->ldisc.receive_buf(tp, tp->flip.char_buf, 
		tp->flip.flag_buf,ldisc_count);
	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
#endif
}

/*
** megamint(mpc)
**
** Process an input control signal transition.  In the usual
** case, a transition indicates a change in the data carrier
** detect.  Hardware flow control is not supported in hardware
** and must be done here if "ctsflow" is enabled.
**
** mpdev board lock ** MUST ** be held			 
*/
static void megamint(register struct mpchan *mpc)
{
	register icpbaddr_t icpb = mpc->mpc_icpb;
	register struct tty_struct  *tp;
	register ushort dcd;
	int old_carr_state;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in megamint()\n");
	}
#endif	/* DEBUG_LOCKS */

	if (mpc->mpc_tty == (struct tty_struct *) NULL)
		return;
	if (!(mpc->mpc_icpi->ssp.cin_attn_ena & ENA_DCD_CNG)){
		return;
	}
	tp = mpc->mpc_tty;
	dcd = icpb->ssp.bank_sigs & CIN_DCD;
	dcd = dcd >> 1;

	old_carr_state = mpc->carr_state;
	if(dcd && (!mpc->carr_state)){
		mpc->carr_state = 1;
		mpc->open_wait_wait = 0;
		wake_up_interruptible(&mpc->open_wait);
#ifdef DEBUG
		printk("waking up device %d\n", SSTMINOR(mpc->mpc_major,
					mpc->mpc_minor));
#endif
	}
	if (!dcd)
		mpc->carr_state = 0;
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	if ((!dcd) && (!((mpc->flags & ASYNC_CALLOUT_ACTIVE) &&
			(mpc->flags & ASYNC_CALLOUT_NOHUP))))
#else
	/* 2.6 kernels */
	if ((!dcd) && (!(mpc->flags & ASYNC_CALLOUT_NOHUP)))
#endif
#ifdef DEBUG
		{
		printk("megamint: hanging up device %d\n", 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
    if (mpc->mpc_icpi->ssp.cin_attn_ena & ENA_DCD_CNG)
		printk("DCD_CNG enabled for device %d\n", 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
		{
#ifdef DEBUG
		printk("megamint: calling tty_hangup for device %d\n", 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
		tty_hangup(tp);
		}
#ifdef DEBUG
		}
#endif
}

/*
** megasint(mpc, flags)
**
** Megaport channel interrupt status register not empty.
** Look for events that are of interest and then clear
** the register.
**
** mpdev board lock ** MUST ** be held			 
** NOTE - lock is dropped and then re-locked to do input 
*/
static void megasint(register struct mpchan *mpc, unsigned long flags)
{
	register struct termios *tiosp;
	struct tty_struct *tty = mpc->mpc_tty;
	char	fbuf, cbuf;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in megasint()\n");
	}
#endif	/* DEBUG_LOCKS */

	if (mpc->mpc_tty == (struct tty_struct *) NULL)
		return;
        tiosp = mpc->mpc_tty->termios;
	if(mpc->mpc_cin_events & EV_BREAK_CNG){	
		if (((tiosp->c_iflag & IGNBRK) == 0) &&
      			(tiosp->c_iflag & BRKINT )){
			if (ISRAMP(mpc)){
      				mpc->mpc_mpa_stat |= MPA_CLR_LOOP_BACK_ERROR;
				mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_ERROR;
				mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;
				mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
				mpc->mpc_mpa_slb_wait = 0;
				mpc->mpc_mpa_clb_wait = 0;
				mpc->mpc_mpa_rst_wait = 0;
				mpc->mpc_mpa_call_back_wait = 0;
      				wake_up_interruptible(&mpc->mpc_mpa_slb);
      				wake_up_interruptible(&mpc->mpc_mpa_clb);
      				wake_up_interruptible(&mpc->mpc_mpa_rst);
      				wake_up_interruptible(&mpc->mpc_mpa_call_back);
			}
		}
#if	(LINUX_VERSION_CODE >= 132624)
		/* 2.6.16+ kernels */
		if (tty->receive_room) {
#else
		if (tty->ldisc.receive_room(tty)) {
#endif
			fbuf = TTY_BREAK;
			cbuf = 0;
			mpc->mpc_break_cnt++;
#ifndef MODULE
			if(mpc->flags & ASYNC_SAK)
				do_SAK(tty);
#endif
#ifdef DEBUG
			printk("megasint: calling n_tty_receive_buf for device %d\n",
				SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
			/*
			** drop mpdev board lock, reacquire after receive_buf
			*/
			spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
			tty->ldisc.receive_buf(tty, &cbuf, &fbuf, 1);
			spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
		}
	}
}

/*
** eqnx_getserial(mpc, sp)
**
** Generate the serial struct info.
*/
static void eqnx_getserial(struct mpchan *mpc, struct serial_struct *sp)
{
	struct serial_struct	sio;

#ifdef DEBUG
	printk("eqnx_getserial(mpc=%p,sp=%p)\n", mpc, sp);
#endif

	memset(&sio, 0, sizeof(struct serial_struct));
	sio.type = PORT_UNKNOWN;
	sio.line = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if (mpc->mpc_mpd->mpd_bus !=  ISA_BUS)
		sio.port = mpc->mpc_mpd->mpd_slot;
	else
		sio.port = ist.ctlr_array[MPBOARD(sio.line)].ctlr_loc;
	sio.irq = 0;
	sio.flags = mpc->flags;
	sio.baud_base = mpc->baud_base;
	sio.close_delay = mpc->close_delay;
	sio.closing_wait = mpc->closing_wait;
	sio.custom_divisor = mpc->custom_divisor;
	sio.xmit_fifo_size = 0;
	sio.hub6 = 0;
	if (eqn_to_user(sp, &sio, sizeof(struct serial_struct))) {
		printk("eqnx_getserial failed copy out\n");
	}

}

/*
** eqnx_setserial(mpc, sp)
**
** Set characteristics according to serial struct info.
*/
static int eqnx_setserial(struct mpchan *mpc, struct serial_struct *sp)
{
	struct serial_struct	sio;
	int win16;
	unsigned long flags;

#ifdef DEBUG
	printk("eqnx_setserial(mpc=%p,sp=%p)\n", mpc, sp);
#endif

	if (eqn_from_user(&sio, sp, sizeof(struct serial_struct)))
		return(-EFAULT);
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */ 
	if (!suser()) {
#else
	/* 2.6+ kernels */
	if (!capable(CAP_SYS_ADMIN)) {
#endif
		if ((sio.baud_base != mpc->baud_base) ||
				(sio.close_delay != mpc->close_delay) ||
				((sio.flags & ~ASYNC_USR_MASK) != (mpc->flags & ~ASYNC_USR_MASK)))
			return(-EPERM);
	} 

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);

	mpc->flags = (mpc->flags & ~ASYNC_USR_MASK) | (sio.flags & ASYNC_USR_MASK);
	mpc->baud_base = sio.baud_base;
	mpc->close_delay = sio.close_delay;
	mpc->closing_wait = sio.closing_wait;
	mpc->custom_divisor = sio.custom_divisor;

	if (mpc->mpc_tty != (struct tty_struct *) NULL) {
		win16 = mpc->mpc_mpd->mpd_nwin;
		if(win16)
			mega_push_win(mpc, 0);
		megaparam(SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
		if(win16)
	   		mega_pop_win(2);
	}
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
	return(0);
}

/*
** eqnx_ioctl(tty, file, cmd, arg)
**
*/
STATIC int eqnx_ioctl(struct tty_struct *tty, struct file * file,
            unsigned int cmd, unsigned long arg)
{
	register struct mpchan *mpc = (struct mpchan *)tty->driver_data;
	struct mpdev *mpd;
	register icpiaddr_t icpi;
	register icpbaddr_t icpb;
	register icpoaddr_t icpo;
	register icpqaddr_t icpq;
	int win16;
	unsigned int arg_int;
	int d, rc = 0;
	unsigned long flags;	

	if (!tty->driver_data)
		return(-ENXIO);                                           

	if ((cmd == EQNXSTTY || cmd == EQNXDTR) &&
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		!suser())
#else
		/* 2.6+ kernels */
		!capable(CAP_SYS_ADMIN))
#endif
			return(-EPERM);

	d = SSTMINOR(mpc->mpc_major, mpc->mpc_minor);
	if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
		|| (d < 0)) 
		return(-ENODEV);

        if ( meg_dev[MPBOARD(d)].mpd_alive == 0)
		return(-ENODEV);
	mpd = mpc->mpc_mpd;

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpd->mpd_lock, flags);

	win16 = mpc->mpc_mpd->mpd_nwin;
	if(win16)
		mega_push_win(mpc, 0);

/* RAMP START --------------------------------------------- */
	/* If RAMP config not completed, don't let any ioctl's in */
	if (ISRAMP(mpc)) {
      /* check break bit */
      /* If not receiving a break, the slot is occupied by a UART */
      
		if ( !(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE) ||
                     (mpc->mpc_mpa_stat & (MPA_INITG_INIT_MODEM | MPA_INITG_MODIFY_SETTINGS))) {
      			icpi = mpc->mpc_icpi;
      			icpb=(rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b 
				: &icpi->ssp.cin_bank_a;
			/* make sure regs are valid */ 
  			if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  				frame_wait(mpc,2); 
         		if (!(icpb->ssp.bank_sigs & 0x01 )) {
				if(win16) mega_pop_win(74);
            			/* Modem present in slot, sleep */
ioctl_modem_wait:
				mpc->mpc_mpa_call_back_wait++;
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
				/* 2.2 and 2.4 kernels */
				interruptible_sleep_on(&mpc->mpc_mpa_call_back);
#else
				/* 2.6 kernels */
				wait_event_interruptible(mpc->mpc_mpa_call_back,
					mpc->mpc_mpa_call_back_wait == 0);
#endif
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				if eqn_fatal_signal {
					spin_unlock_irqrestore(&mpd->mpd_lock, 
							flags);
					return(-ERESTARTSYS);
				}
				if(mpc->mpc_mpa_stat & MPA_CALL_BACK_ERROR){
					spin_unlock_irqrestore(&mpd->mpd_lock, 
							flags);
                        		return(-ENXIO);
				}
				else  if ( !(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE) ||
                     (mpc->mpc_mpa_stat & (MPA_INITG_INIT_MODEM | MPA_INITG_MODIFY_SETTINGS))) {
					goto ioctl_modem_wait;
				}
				if(win16)
					mega_push_win(mpc, 0);
         		} else {
				if(win16) mega_pop_win(75);
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
            			/* No modem, return error */
				return(-ENXIO);
         		}
		}
	}
/* RAMP END ----------------------------------------------- */
	icpi = mpc->mpc_icpi;
	icpo = mpc->mpc_icpo;
	icpq = &icpo->ssp.cout_q0;
	spin_unlock_irqrestore(&mpd->mpd_lock, flags);

#ifdef DEBUG
	printk("eqnx_ioctl: cmd %x for device %d\n", (unsigned int) cmd, 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
	switch (cmd) {
	case TCSBRK:		/* SVID version: non-zero arg --> no break */
		if ((rc = tty_check_change(tty)) == 0) {
			tty_wait_until_sent(tty, 0);
			if (! arg){
				/* send break */
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				if(ISRAMP(mpc))
            				ramp_start_break(mpc);
				else
					icpo->ssp.cout_flow_cfg |= 0x20; 
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);

				eqnx_delay(HZ/4);	/* 1/4 second */

				/* stop break */
				spin_lock_irqsave(&mpd->mpd_lock, flags);
				if(ISRAMP(mpc))
            				ramp_stop_break(mpc);
				else
					icpo->ssp.cout_flow_cfg &= ~0x20; 
				spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			}
		}
		break;
	case TCSBRKP:
		if ((rc = tty_check_change(tty)) == 0) {
			tty_wait_until_sent(tty, 0);
			/* send break */
			spin_lock_irqsave(&mpd->mpd_lock, flags);
			if(ISRAMP(mpc))
            			ramp_start_break(mpc);
			else
				icpo->ssp.cout_flow_cfg |= 0x20; 
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);

			eqnx_delay( arg ? arg*(HZ/10) : HZ/4);

			/* stop break */
			spin_lock_irqsave(&mpd->mpd_lock, flags);
			if(ISRAMP(mpc))
            			ramp_stop_break(mpc);
			else
				icpo->ssp.cout_flow_cfg &= ~0x20; 
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		}
		break;
	case TIOCGSOFTCAR:
		put_fs_int(((tty->termios->c_cflag & CLOCAL) ? 1 : 0), 
				(unsigned int *) arg);
		break;
	case TIOCSSOFTCAR:
#if (EQNX_VERSION_CODE < 131328)  
		arg_int = get_fs_long((unsigned long *) arg);
#else
		get_user(arg_int, (unsigned int *) arg);
#endif
		tty->termios->c_cflag = (tty->termios->c_cflag & ~CLOCAL) | 
			(arg_int ? CLOCAL : 0);
		break;
	case TIOCMGET:
		rc = get_modem_info(mpc, (unsigned int *) arg);
		break;
	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCMSET:
		rc = set_modem_info(mpc, cmd, (unsigned int *) arg, tty);
		break;
	case TIOCGSERIAL:
		eqnx_getserial(mpc, (struct serial_struct *) arg);
		break;
	case TIOCSSERIAL:
		rc = eqnx_setserial(mpc, (struct serial_struct *) arg);
		break;
	case EQNXSTTY:		/* change parameters */
		rc = megastty(mpc, tty, arg, d);
		break;
	case EQNXDTR: {
		icpoaddr_t icpo; 
   		uchar_t cur;
		mpc = &meg_chan[d];

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
		win16 = mpc->mpc_mpd->mpd_nwin;
		if(win16)
		   mega_push_win(mpc, 0);
		icpo = mpc->mpc_icpo;
		GET_CTRL_SIGS(mpc, cur);
      		if (arg == 1) /* unconditionally raise dtr */ 
			cur |= (TX_DTR | TX_HFC_DTR);
		else /* unconditionally drop dtr */ 
			cur &= ~(TX_DTR | TX_HFC_DTR);
		if ((icpo->ssp.cout_int_opost & 0x8) && (cur & TX_SND_CTRL_TG))
			cur ^= TX_SND_CTRL_TG;
		if (!(icpo->ssp.cout_int_opost & 0x8) && !(cur & TX_SND_CTRL_TG))  
			cur ^= TX_SND_CTRL_TG;
		SET_CTRL_SIGS(mpc, cur);
		if(win16)
		   mega_pop_win(26);
		}
		spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
		break;
	default:
		rc = -ENOIOCTLCMD;
		break;
	}
	if(win16) {
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		mega_pop_win(35);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
	}
	return(rc);
}

/*
** eqn_cs_get(mpc)
**
** Return the current set of input and output control signals.
**
** The value returned is "native" icp bit positions regardless of the 
** origin of the signals.  Namely,
**
**     b0      dcd
**     b1      cts
**     b2      dsr
**     b3      ri
**     -----------
**     b4      dtr
**     b5      rts
**     b6      0
**     b7      0
**
** The returned bits are POSITIVE logic... 1 means signal is on.
**
** mpdev board lock ** MUST ** be held			 
*/
static uchar_t eqn_cs_get( struct mpchan *mpc)
{
	uchar_t ret_val = 0; /* default */
	register icpiaddr_t icpi;
	volatile union bank_regs_u *bank;
	ushort_t rawcs;

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in eqn_cs_get()\n");
	}
#endif	/* DEBUG_LOCKS */

  	icpi = mpc->mpc_icpi;
	bank = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
        if (!(bank->ssp.bank_events & EV_REG_UPDT)) 
        	frame_wait(mpc,2); /* make sure regs are valid */ 

	/* get INPUT control signals */
	rawcs = (bank->ssp.bank_sigs >> 1) & 0x0f;
	ret_val = rawcs & 0xff;

	/* get OUTPUT control signals */
	if ( mpc->mpc_cout->ssp.cout_int_flow_ctrl & 0x04 ){
		/* input overload signals sent to lmx */
		GET_CTRL_SIGS(mpc, rawcs);
		rawcs = rawcs >> 4;
	} else
		/* normal signals sent to lmx */
		GET_CTRL_SIGS(mpc, rawcs);

	/* combine in signals and out signals */
	ret_val |= ((rawcs & 0x0f) << 4 );
	return ret_val;
}  /* eqn_cs_get */

/*
** get_modem_info(mpc, value)
**
*/
static int get_modem_info(struct mpchan *mpc, unsigned int *value)
{
	unsigned char status;
	unsigned int result = 0;
	unsigned long flags;

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	status = eqn_cs_get(mpc);
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);

	result = (((status & 0x20) ? TIOCM_RTS : 0)
		| ((status & 0x10) ? TIOCM_DTR : 0)
		| ((status & 0x1) ? TIOCM_CAR : 0)
		| ((status & 0x8) ? TIOCM_RI : 0)
		| ((status & 0x4) ? TIOCM_DSR : 0)
		| ((status & 0x2) ? TIOCM_CTS : 0));

	put_fs_int(result, (unsigned int *) value);
	return(0);
}

/*
** set_modem_info(mpc, cmd, value, tty)
**
*/
static int set_modem_info(struct mpchan *mpc, unsigned int cmd,
		unsigned int *value, struct tty_struct *tty)
{
	unsigned int arg, temp;
	icpoaddr_t icpo = mpc->mpc_icpo;
	unsigned long flags;
	struct termios *term = tty->termios;

#if (EQNX_VERSION_CODE < 131328)  
	arg = get_fs_long((unsigned long *) value);
#else
	get_user(arg, (unsigned int *) value);
#endif

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	GET_CTRL_SIGS(mpc, temp);
	switch (cmd) {
	case TIOCMBIS: 
		if (arg & TIOCM_DTR)
			temp |= (TX_DTR | TX_HFC_DTR);
		/* do not change RTS if CRTSCTS is on */
		if ((arg & TIOCM_RTS) && (term) && ((term->c_cflag & CRTSCTS) == 0))
			temp |= (TX_RTS | TX_HFC_RTS);
		break;
	case TIOCMBIC: 
		if (arg & TIOCM_DTR)
			temp &= ~(TX_DTR | TX_HFC_DTR);
		/* do not change RTS if CRTSCTS is on */
		if ((arg & TIOCM_RTS) && (term) && ((term->c_cflag & CRTSCTS) == 0))
			temp &= ~(TX_RTS | TX_HFC_RTS);
		break;
	case TIOCMSET: 
		if (arg & TIOCM_DTR)
			temp |= (TX_DTR | TX_HFC_DTR);
		else
			temp &= ~(TX_DTR | TX_HFC_DTR);
		/* do not change RTS if CRTSCTS is on */
		if ((term) && ((term->c_cflag & CRTSCTS) == 0)) {
			if (arg & TIOCM_RTS)
				temp |= (TX_RTS | TX_HFC_RTS);
			else
				temp &= ~(TX_RTS | TX_HFC_RTS);
		}
		break;
	}
	if ( ((icpo->ssp.cout_int_opost & 0x8) && (temp & TX_SND_CTRL_TG)) ||
	   (!(icpo->ssp.cout_int_opost & 0x8) && !(temp & TX_SND_CTRL_TG)) )  
			temp ^= TX_SND_CTRL_TG;
	SET_CTRL_SIGS(mpc, temp);
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
	return(0);
}

/*
** megastty(mpc, tty, arg, dev)
**
** overwrite stty setup 
*/
static int megastty( struct mpchan *mpc, struct tty_struct *tty,

		int arg, int dev)
{
	int win16;
	int rc = 0;
	struct termios *tp = tty->termios;
	struct marb_struct *curmarb;
	struct lmx_struct *lmx;
	int slot_chan, port, ldv;
	unsigned long flags;
	struct mpdev *mpd;
	
  /*
  ** lock mpdev board lock
  */
  spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);

  switch( arg ) {
  case 1:
	mpc->mpc_param |= IXONSET;	/* force ixon */
	tp->c_iflag |= IXON;
	megaparam(dev);
	break;

  case 2:
	mpc->mpc_param &= ~IXONSET;	/* cancel forcing ixon */
	tp->c_iflag &= ~IXON;
	megaparam(dev);
	break;

  case 3:
	mpc->mpc_param |= IXANYIG;	/* force ignore ixany */
	megaparam(dev);
	break;

  case 4:
	mpc->mpc_param &= ~IXANYIG;	/* cancel ignore ixany */
	megaparam(dev);
	break;

  case 17:
	tp->c_cflag |= CRTSCTS;
	if((mpc->mpc_tty->termios->c_cflag & RTSFLOW) == 0) {
		if((tp->c_cflag & CRTSCTS) == 0)
		   rc = -EINVAL;
		else
		   mpc->mpc_param |= IOCTRTS;
	}
	megaparam(dev);
	break;

  case 18:
	tp->c_cflag &= ~CRTSCTS;
	mpc->mpc_param &= ~IOCTRTS;
	megaparam(dev);
	break;

  case 21:
	mpd = mpc->mpc_mpd;
	mpc->mpc_param |= IOCTLLB;
		/* int/ext loopback, command enable */
/* RAMP START --------------------------------------------- */
	   if (ISRAMP(mpc)) {
      	   	lmx = &(mpc->mpc_icp->lmx[mpc->mpc_lmxno]);
	   	win16 = mpc->mpc_mpd->mpd_nwin;
	   	if(win16)
		   mega_push_win(mpc, 0);
			/* 64 ports per ICP, 16 ports per lmx */
 		ldv = ((mpc - meg_chan) % 64) / 16; 
 		port = (mpc - meg_chan) % MAXLMXMPC;
 		slot_chan = port + (ldv * 16);
         	curmarb = &((mpc->mpc_icp->slot_ctrl_blk + slot_chan)->marb);
#ifdef DEBUG
printk("calling ramp_set_loop_back for device %d\n", mpc->mpc_chan);
#endif
         	if (ramp_set_loop_back(mpc)) {
            		if(win16)
               			mega_pop_win(51);
	   		rc = -ENXIO;
			break;
         	}
	        if(win16)
		         mega_pop_win(52);
loopback_21:
		mpc->mpc_mpa_slb_wait++;
  		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		interruptible_sleep_on(&mpc->mpc_mpa_slb);
#else
		/* 2.6 kernels */
		wait_event_interruptible(mpc->mpc_mpa_slb,
			mpc->mpc_mpa_slb_wait == 0);
#endif
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		if eqn_fatal_signal {
			rc = -ERESTARTSYS;
			break;
		}
		if (mpc->mpc_mpa_stat & MPA_SET_LOOP_BACK_ERROR){
	   		rc = -ENXIO;
		}
		else if (mpc->mpc_mpa_stat == MPA_INITG_SET_LOOP_BACK)
			goto loopback_21;
			
	} else {
/* RAMP END ----------------------------------------------- */
		/* int/ext loopback, command enable */
		mpc->mpc_cout->ssp.cout_lmx_cmd |= 0x43;
		frame_wait( mpc, 6 );
		/* clear cmd enable */
		mpc->mpc_cout->ssp.cout_lmx_cmd &= ~0x40;
		mpc->mpc_flags |= MPC_LOOPBACK;
	}
	   break;


  case 22:
	mpd = mpc->mpc_mpd;
	mpc->mpc_param &= ~IOCTLLB;
/* RAMP START --------------------------------------------- */
	if (ISRAMP(mpc)) {
		win16 = mpc->mpc_mpd->mpd_nwin;
      		lmx = &(mpc->mpc_icp->lmx[mpc->mpc_lmxno]);
		if(win16)
	   		mega_push_win(mpc, 0);
		/* 64 ports per ICP, 16 ports per lmx */
 		ldv = ((mpc - meg_chan) % 64) / 16; 
 		port = (mpc - meg_chan) % MAXLMXMPC;
 		slot_chan = port + (ldv * 16);
         	curmarb = &((mpc->mpc_icp->slot_ctrl_blk + slot_chan)->marb);
         	if (ramp_clr_loop_back(mpc)) {
			if(win16)
				mega_pop_win(6);
	   		rc = -ENXIO;
			break;
		}
		if(win16)
			mega_pop_win(6);
loopback_22:
		mpc->mpc_mpa_clb_wait++;
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		interruptible_sleep_on(&mpc->mpc_mpa_clb);
#else
		/* 2.6 kernels */
		wait_event_interruptible(mpc->mpc_mpa_clb, 
			mpc->mpc_mpa_clb_wait == 0);
#endif
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		if eqn_fatal_signal {
			rc = -ERESTARTSYS;
			break;
		}
		if (mpc->mpc_mpa_stat & MPA_CLR_LOOP_BACK_ERROR){
	   		rc = -ENXIO;
		}
		else if (mpc->mpc_mpa_stat == MPA_INITG_CLR_LOOP_BACK)
			goto loopback_22;
	} else {
/* RAMP END ----------------------------------------------- */
		mpc->mpc_cout->ssp.cout_lmx_cmd &= ~0x03;       /* int/ext loopback */
		mpc->mpc_cout->ssp.cout_lmx_cmd |= 0x40;        /* command enable */
		frame_wait( mpc, 6 );
		/* clear cmd enable */
		mpc->mpc_cout->ssp.cout_lmx_cmd &= ~0x40;
		mpc->mpc_flags &= ~MPC_LOOPBACK;
	}
	break;

  case 23:
	   mpc->mpc_param |= IOCTXON;
	   megaparam(dev);
	   break;

  case 24:
	   mpc->mpc_param &= ~IOCTXON;
	   megaparam(dev);
	   break;

  case 25:
	   mpc->mpc_param |= IOCTLCK;
	   break;

  case 26:
	   mpc->mpc_param &= ~IOCTLCK;
	   break;
  default: ;
  }

  spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
  return(rc);
}

/*
** move_brd_addr(addr)
**
*/
int move_brd_addr( unsigned long int addr)
{
register struct mpdev *mpd;
register struct mpchan *mpc;
volatile unsigned char *chnl_ptr;

int ret=true;
int ram_ok=false;
int ii, jj,j, i;
paddr_t mpd_mem = 0;
int z = 0,y = 0;
uchar_t cchnl;
ushort_t no_cache;
icpgaddr_t icpg;
int win16;
mpaddr_t mp;
int k;
int first_time=0;
int rerror=0;
unsigned long flags;

   if (io_bus_type < 1 || io_bus_type > 2)
   {
       printk("Eqnx: Invalid Bus type (%x)\n",io_bus_type);
       return(-EFAULT);
   }
    /* all ISA brd that are alive */
   for (k=0 ; k < nmegaport; k++)
   {

         mpd = &meg_dev[k];
         win16 = mpd->mpd_nwin;
         /* check if brd is page and ISA
            otherwise an error
         */

         if (!win16   || mpd->mpd_bus != ISA_BUS  || mpd->mpd_alive== false)
           continue;
         if (mpd->mpd_pmem == addr)
		return(rerror);

       /*
          Check if memory address were in an 0xFF if so error
          check if given addr is valid in table
          0xfe or 0xf1  fir only the first isa brd , since all are map to
          same window
       */

       if (!first_time)
       {
            for (z=0; z< HOLE_TABLE_SIZE; z++)
            {
                    if (ist.sys_isa_holes[z] == 0)
                           continue;
                    if (ist.sys_isa_holes[z] == mpd->mpd_pmem)
                    {


                        if (ist.sys_isa_hole_status[z] == 0xff)
                        {
                              printk("Eqnx: Warning move brd at hole %d to the 16 bit addr %x\n",z, (unsigned int) addr);
                              ram_ok=true;
                              break;
                        }
                        else
                         ram_ok=true;
                      break;
                  }
            }
            if (ram_ok )
            {
                ram_ok=false;
                for (y=0; y< HOLE_TABLE_SIZE; y++)
                {
                        if (ist.sys_isa_holes[y] == 0)
                               continue;
                        if (ist.sys_isa_holes[y] == addr)
                        {

                            if (ist.sys_isa_hole_status[y] == 0xf1 ||
                                   ist.sys_isa_hole_status[y] == 0xf2 ||
                                   ist.sys_isa_hole_status[y] == 0xf3) 
                            {

                                  ram_ok=true;
                                  break;
                            }
                          break;
                      }
                }

           }
           if (!ram_ok)
           {
               printk("EQNX: Invalid Memory Address (%x)\n",
			(unsigned int) addr);

               ret=-EFAULT;
               return(ret);
            }
    }
/* Pat */
#if (EQNX_VERSION_CODE < 131328)
	if (addr > 0x100000){
#endif
/* Pat */       
		if((mpd_mem = (paddr_t) ioremap(addr,0x4000)) == (paddr_t) NULL){
			printk("EQUINOX: Failed to allocate virtual address space of size %d\n",
				0x4000);
		}
/* Pat */
#if (EQNX_VERSION_CODE < 131328)
	}
	else
		mpd_mem = addr;
#endif
/* Pat */		

        if(!mpd_mem)
        {

	   printk("EQNX: Memory mapping failed for board %d\n", k +1);
           ret= ENOMEM;
           return(ret);
        }

	/*
	** lock mpdev board lock
	*/
	spin_lock_irqsave(&mpd->mpd_lock, flags);
        if(win16)
           mega_push_winicp(mpd, 0);

    	if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
       		/* Set the address, 16-bit mode, MAX2, 
		 * and flat off 
		 */ 
       		outb(((addr >> 14) & 0xff), portn[mpd->mpd_io]);
       		outb((((addr >> 22) & 3)| 0x20),portn[mpd->mpd_io] + 1);
       		/* page register - deselect ram */  
		
       		outb(0, portn[mpd->mpd_io] + 2);
       		outb(0, portn[mpd->mpd_io] + 3);

       		/* disable interrupts */ 
       		outb(0, portn[mpd->mpd_io] + 6);
    	}
    	else { /* SSP4 board */
      		unsigned char b;
    		
      		/* reg 8 (page reg) */
      		if(mpd->mpd_nwin == HA_FLAT)
         		b = 0x20;
      		else
         		b = 0;

      		outb(b, portn[mpd->mpd_io] + 0x08);

      		b = ((addr & 0x0000c000) >> 8);
      		/* no ints */
      		outb(b,portn[mpd->mpd_io] + 0x09);
      		b =((addr & 0xFFFF0000) >> 16);
      		outb(b,portn[mpd->mpd_io] + 0x0a);
      		/* reg b*/
      		b=0;
      		outb(b, portn[mpd->mpd_io] + 0x0b);

    	} /* SSP4 */
    
            /*  1 icps */
             j=0;
    
    /* select correct page if necessary */
    if ( mpd->mpd_nwin == HA_WIN16 ){
      if(mpd->mpd_board_def->asic == SSP64)
	/* SSP64 */      
        outw( 0x0100 | j, portn[mpd->mpd_io] + 2);
      else
	/* SSP2 / SSP4 */
        outb( 1 << j, portn[mpd->mpd_io] + 8);
    }

    if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */
        icpg = (icpgaddr_t)((unsigned long)mpd_mem + 0x2000);
       /* verify that pram is not cached */

      cchnl = Gicp_chnl;
      chnl_ptr = &(Gicp_chnl);
    }
    else { /* SSP4 board */
      icpaddr_t icp;
      volatile union global_regs_u *icp_glo ;
      icp = &mpd->icp[j];
      icp_glo = (volatile union global_regs_u *)((unsigned long)mpd_mem + 0x400);
      cchnl = icp_glo->ssp2.chan_ctr;
      chnl_ptr = &(icp_glo->ssp2.chan_ctr);
    }
      no_cache = false;
      ret=true;
      for ( ii = 0; ii < 0x10000; ii++ )
        if ( *chnl_ptr != cchnl )
        {
          no_cache = true;
          break;
        }
      if ( !no_cache )
      {
          printk("EQNX: PRAM memory appears to be cached %d for with I/O address 0x%x.\n",
          ii,portn[mpd->mpd_io]);
         ret=false;
         rerror=-EFAULT;
      }
      else
        rerror=0;

      if (ret == false )
      {
        /* if failure map back to org addr */
        if (mpd->mpd_board_def->asic == SSP64) { /* SSP64 board */

        /*Disable Controller's addressing */
        outw( 0x0000, portn[mpd->mpd_io] + 2);
        outw( 0x0000, portn[mpd->mpd_io]);

        /* Set the address, 16-bit mode, MAX2, and flat off */
        outb((mpd->mpd_pmem >> 14) & 0xff, portn[mpd->mpd_io]);
        outb((((mpd->mpd_pmem >> 22) & 3)| 0x20), portn[mpd->mpd_io] + 1);

        /* page register - deselect ram */
        outb(0, portn[mpd->mpd_io] + 2);
        outb(0, portn[mpd->mpd_io] + 3);

        /* disable interrupts */
        outb(0, portn[mpd->mpd_io] + 6);
        }
        else { /* SSP4 board */
          unsigned char b;
     
          /* reg 8 (page reg) */
          if(mpd->mpd_nwin == HA_FLAT)
             b = 0x20;
          else
             b = 0;

          outb( b, portn[mpd->mpd_io] + 0x08);

          b = ((mpd->mpd_pmem & 0x0000c000) >> 8);
          /* no ints */
          outb(b, portn[mpd->mpd_io] + 0x09);
          b = ((mpd->mpd_pmem & 0xFFFF0000) >> 16);
          outb(b, portn[mpd->mpd_io] + 0x0a);
          /* reg b*/
          b=0;
          outb(b,portn[mpd->mpd_io] + 0x0b);
        }

/* Pat */
#if (EQNX_VERSION_CODE < 131328)
	if (mpd_mem > 0x100000)
#endif
/* Pat */
		iounmap((void *) mpd_mem);

      }
      else
      {
       /* free old alloc */
/* Pat */
#if (EQNX_VERSION_CODE < 131328)
	if (mpd->mpd_mem > 0x100000)
#endif
/* Pat */	
		iounmap((void *) mpd->mpd_mem);

         /* change status setting in hold table
            for first success
         */

        if (!first_time)
        {
           switch (ist.sys_isa_hole_status[z])
           {
              case 0xfd:
                ist.sys_isa_hole_status[z] = 0xf1;    /* old */
                break;

              case 0xff:
                 ist.sys_isa_hole_status[z] = 0xf3;    /* old */
                break;

              case 0xfe:
              default:
                ist.sys_isa_hole_status[z] = 0xf1;    /* old */
                break;
           }

           /* set new status */
           switch (ist.sys_isa_hole_status[y])
           {

              case 0xf2:
                ist.sys_isa_hole_status[y] = 0xfd;    /* new */
                break;
              case 0xf3:
                ist.sys_isa_hole_status[y] = 0xff;    /* new */
                break;

              case 0xf1:
              default:
                ist.sys_isa_hole_status[y] = 0xfe;    /* new */
                break;
           }

           first_time=1;
        }
           printk("EQNX: Moved board(%d) from addr(%lx) to addr(%lx) ok \n",
                    k,(unsigned long) mpd->mpd_pmem,(unsigned long) addr);

	   mpd->mpd_mem= mpd_mem;
	   mpd->mpd_pmem= addr;

/* This should be unneccesary but playing it safe... */
	if(mpd->mpd_board_def->asic == SSP64)
		mpd->mpd_sspchan = NCHAN; /* SSP64 */
	else
		mpd->mpd_sspchan = 4; /* SSP4 */ 
/* end playing it safe */
	brd_mem_cfg(mpd);

          for(j=0; j < (int)mpd->mpd_nicps; j++)
          {
              mp = (mpaddr_t)mpd->icp[j].icp_regs_start;

            /* loop thru each channel - all bytes have been zero'd */
            for ( i = 0; i <  MIN(MAXICPCHNL, mpd->mpd_sspchan); i++ )
            {
                jj = mpd->mpd_minor + i + (j * mpd->mpd_sspchan);
                mpc = &meg_chan[jj];

                mpc->mpc_icpi = (icpiaddr_t)&mp->mp_icpi[i];
            	if (mpd->mpd_board_def->asic == SSP64)
			/* SSP64 */
                	mpc->mpc_icpo = (icpoaddr_t)&mp->mp_icpo[i];
		else
			/* SSP2 / SSP4 */
                	mpc->mpc_icpo = (icpoaddr_t)((unsigned char *)
				(mpc->mpc_icpi) + 0x200);
                mpc->mpc_icp = (icpaddr_t)&mpd->icp[j];
     		mpc->mpc_rxq.q_addr = (char *)(mpc->mpc_icp->icp_dram_start);
		if (mpd->mpd_board_def->asic == SSP64) 
			/* SSP64 */
     			mpc->mpc_txq.q_addr = (char *)(mpc->mpc_icp->icp_dram_start);
		else
			/* SSP2 / SSP4 */
     			mpc->mpc_txq.q_addr = (char *)(mpc->mpc_icp->icp_dram_start +
						0x1000);
            }
         }
    }

    if (win16)
      mega_pop_win(48);

    spin_unlock_irqrestore(&mpd->mpd_lock, flags);
  }

  return(rerror);
}

/*
** eqnx_diagioctl(ip, fp, cmd, arg)
**
*/
STATIC int eqnx_diagioctl(struct inode *ip, struct file *fp, unsigned int cmd,
            unsigned long arg)
{
	int nchan,d;
	struct mpchan *mpc;
	struct mpdev *mpd;
	register icpiaddr_t icpi;
	struct termios *tp;
	int rc = 0, minor;
	unsigned int length;
	int win16;
	ushort_t cur;
	unsigned long flags;

#ifdef DEBUG
	printk("eqnx_diagioctl: cmd %x for device %d\n", (unsigned int) cmd, 
		MINOR(ip->i_rdev));
#endif
	if ((cmd == EQNSTATUS1) && 
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */ 
		!suser()) {
#else
		/* 2.6+ kernels */
		!capable(CAP_SYS_ADMIN)) {
#endif
			return(-EPERM);
	}
	minor = MINOR(ip->i_rdev);
	switch (minor){
	case 0:
	switch (cmd){
      	case EQNTRACEON:
		eqn_ramp_trace = true;
		break;

      	case EQNTRACEOFF:
		eqn_ramp_trace = false;
		break;

	case EQNQUIETOFF:
		eqn_quiet_mode = false;
		eqn_save_trace = true;
		break;
	case EQNQUIETON:
		eqn_quiet_mode = true;
		eqn_save_trace = false;
		break;
	case EQNRESUME:			/* resume output */
					/* min number passed in as arg */
		if (eqn_from_user(&length, (unsigned int *)arg, 
			sizeof(unsigned int))) {
			return(-EFAULT);
		}

		d = SSTMINOR_FROMDEV(length);
		if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
			|| (d < 0)) {
			return(-ENODEV);
		}
        	mpc = &meg_chan[d];
		if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
			return(-ENODEV);
		}
		nchan = MPCHAN(d);
		if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
			nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
		}

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpd->mpd_lock, flags);

		win16 = mpc->mpc_mpd->mpd_nwin;
		if(win16)
			mega_push_win(mpc, 0);
		eqnx_flush_buffer_locked(mpc->mpc_tty);
		icpi = mpc->mpc_icpi;
		icpi->ssp.cin_inband_flow_ctrl &= ~0x01;
		if(win16)
			mega_pop_win(35);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);

		if (write_wakeup_deferred) {
			write_wakeup_deferred = 0;
			mpc->mpc_tty->ldisc.write_wakeup(mpc->mpc_tty);
		}

		break;
	case EQNISTCFG:
		if (eqn_to_user((struct ist_struct *) arg, &ist, sizeof(ist))) {
			return(-EFAULT); 
		}
		
		break;
	case EQNSTATUS2:{
		struct SttyStatus sttystatus;
		if (eqn_from_user(&sttystatus, (struct SttyStatus *)arg, 
			sizeof(struct SttyStatus))) {
			return(-EFAULT); 
		}

		d = SSTMINOR_FROMDEV(sttystatus.mp_dev);
		if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
			|| (d < 0)) {
			return(-ENODEV);
		}
        	mpc = &meg_chan[d];
		if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
			return(-ENODEV);
		}
		nchan = MPCHAN(d);
		if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
			nchan >= (int)mpd->mpd_nchan){
			return(-ENODEV);
		}

		sttystatus.mpc_param = mpc->mpc_param;
		if (eqn_to_user((int *) arg, &sttystatus, 
			sizeof(sttystatus))) {
			 return(-EFAULT);
		}
		}
		break;
	case EQNSTATUS3: { 	/* status3 */
		struct ModemStatus mdstatus;
		if (eqn_from_user(&mdstatus, (struct ModemStatus *)arg, 
			sizeof(struct ModemStatus))) {
			return(-EFAULT);
		}

		d = SSTMINOR_FROMDEV(mdstatus.mp_dev);
		if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
			|| (d < 0)) {
			return(-ENODEV);
		}
        	mpc = &meg_chan[d];
		if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
			return(-ENODEV);
		}
		nchan = MPCHAN(d);
		if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
			nchan >= (int)mpd->mpd_nchan){
			return(-ENODEV);
		}

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		mdstatus.mpstatus = eqn_cs_get(mpc);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		if (eqn_to_user((struct ModemStatus *) arg, &mdstatus, 
			sizeof(mdstatus))) {
			return(-EFAULT); 
		}
		
		break;
	}
	case EQNSTATUS1: { 	/* status1 */
		struct disp dp;
		icpaddr_t  icp;
		icpiaddr_t icpi;
		icpoaddr_t icpo;
		icpgaddr_t icpg;
		icpqaddr_t icpq;
		icpbaddr_t icpb;
		if (eqn_from_user(&dp, (unsigned char *)arg, sizeof(dp))) {
			return(-EFAULT);
		}
			
		d = SSTMINOR_FROMDEV(dp.mp_dev);
		if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
			|| (d < 0)) {
			return(-ENODEV);
		}
        	mpc = &meg_chan[d];
		if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
			return(-ENODEV);
		}
		nchan = MPCHAN(d);
		if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
			nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
		}
		/* verify version */
		if (dp.ioctl_version != IOCTL_VERSION) {
			return (-EINVAL);
		}

		/*
		** lock mpdev board lock
		*/
		dp.mp_outqcc = eqnx_write_room(mpc->mpc_tty);

		spin_lock_irqsave(&mpd->mpd_lock, flags);

		win16 = mpc->mpc_mpd->mpd_nwin;
		if(win16)
			mega_push_win(mpc, 0);

		dp.ioctl_version = IOCTL_VERSION;
		dp.mp_state = mpc->flags;
		if (mpc->flags & ASYNC_INITIALIZED){
			tp = mpc->mpc_tty->termios;
			dp.mp_iflag = tp->c_iflag;
			dp.mp_oflag = tp->c_oflag;
			dp.mp_lflag = tp->c_lflag;
			dp.mp_cflag = tp->c_cflag;
			dp.mp_ldisp = tp->c_line;
		}
		dp.mp_param = mpc->mpc_param;
		dp.mp_flags = mpc->mpc_flags;
		dp.mp_mpa_stat = mpc->mpc_mpa_stat;
		icp = mpc->mpc_icp;
		icpi = mpc->mpc_icpi;
		icpo = mpc->mpc_icpo;
		if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)
			/* SSP64 */
			icpg = (icpgaddr_t)icpo;
		else
			/* SSP2 / SSP4 */
			icpg = (icpgaddr_t)((unsigned long)icpi + 0x400);
		icpq = &icpo->ssp.cout_q0;
		icpb = (rx_locks & LOCK_A) 
			? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
		dp.mp_tx_count = mpc->mpc_count;
		dp.mp_tx_base = Q_data_base;
		dp.mp_tx_q_ptr = Q_data_ptr;
		dp.mp_rawqcc = dp.mp_tx_q_cnt = Q_data_count;
		dp.mp_rx_base = rx_base;
		dp.mp_rx_next = rx_next;
		dp.mp_rx_tail = mpc->mpc_rxq.q_ptr;
		dp.mp_rx_fmt = rx_param;
		dp.mp_rxbaud = rx_baud;
		dp.mp_cin_ctrl = rx_ctrl_sig;
		dp.mp_lmx_cmd = icpo->ssp.cout_lmx_cmd;
		dp.mp_tx_fmt = tx_char_fmt;
		dp.mp_txbaud = tx_baud;
		dp.mp_cout_ctrl = tx_ctrl_sig;
		dp.mp_rx_csr = rx_csr;
		dp.mp_tx_csr = tx_csr;
		dp.mp_tx_flow_cfg = tx_flow_config;
		dp.mp_susp_lmx = rx_suspo_lmx;
		dp.mp_susp_sig = rx_suspo_sig;
		dp.mp_rx_locks = rx_locks;
		dp.mp_cie = rx_cie;
		dp.mp_cis = rx_events | mpc->mpc_cin_events;
		dp.mp_cieo = tx_cie;
		if(tx_csr & TXSR_EV_B_ACT) 
		   dp.mp_tx_events = icpo->ssp.cout_events_b; 
		else
		   dp.mp_tx_events = icpo->ssp.cout_events_a; 
		dp.mp_rxtdm = rx_tdm_early;
		dp.mp_txtdm = 0;
		dp.mp_int_flgs = icpi->ssp.cin_int_flags;
		dp.mp_vmin = rx_vmin;
		dp.mp_vtime = rx_vtime;
		dp.mp_fifo_cnt = rx_fifo_cnt;
		dp.mp_rx_rcv_cnt = rx_rcv_cnt;
		dp.mp_tx_cpu_reg = tx_cpu_reg;
		dp.mp_start = mpc->mpc_start;
		dp.mp_stop = mpc->mpc_stop;
		dp.mp_input = mpc->mpc_input;
		dp.mp_output = mpc->mpc_output;
		dp.mp_bus = mpc->mpc_mpd->mpd_bus;
		dp.mp_brdno = mpc->mpc_brdno;
		dp.mp_icpno = mpc->mpc_icpno;
		dp.mp_lmxno = mpc->mpc_lmxno;
		dp.mp_chan = mpc->mpc_chan;
		dp.mp_parity_err_cnt = mpc->mpc_parity_err_cnt;
		dp.mp_framing_err_cnt = mpc->mpc_framing_err_cnt;
		dp.mp_break_cnt = mpc->mpc_break_cnt;
		
		if (mpc->mpc_mpd->mpd_board_def->asic == SSP64)  {
			/* SSP64 */
			dp.mp_g_attn= i_gicp_attn;
			dp.mp_g_init = Gicp_initiate;
		}
		else {
			/* SSP2 / SSP4 */
			dp.mp_g_attn= icpg->ssp2.attn_pend;
			dp.mp_g_init = 0;
		}
		dp.mp_g_rx_attn0 = gicp_rx_attn[0];
		if(win16)
		   mega_pop_win(8);
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		if (eqn_to_user((unsigned char *) arg, &dp, sizeof(dp)))
			rc = -EFAULT;
			
		break;
	} /* End STATUS1 */
	case EQNMOVEBRD:{
		struct movebrd mb;
		if (eqn_from_user(&mb, (struct movebrd *) arg, 
			sizeof(struct movebrd))) {
			rc = -EFAULT;
			break;
		}

		if (!mb.addr)
			rc = -EINVAL;
		else
			rc = move_brd_addr(mb.addr);
		}
		break;
      case EQNBRDSTATUS: {
			struct	eqnbrdstatus	brdstat;
			int	index, i, j, version;

			if (eqn_from_user(&index, (unsigned int *) arg, 
				sizeof(unsigned int))) {
				rc = -EFAULT;
				break;
			}

			/* Board number is expected to start at 1 (not zero) */
			if(index > nmegaport || index < 1) {
				rc = -EINVAL;
				break;
			}

			/* verify version */
			if (eqn_from_user(&version, (unsigned int *) arg+1, 
				sizeof(unsigned int))) {
				rc = -EFAULT;
				break;
			}

			if(version != IOCTL_VERSION) {
				rc = -EINVAL;
				break;
			}
			
			/* build eqnbrdstatus structure */

			brdstat.brd_nbr = index;
			brdstat.ioctl_version = IOCTL_VERSION;
			index--; 	/* bias board for internal indices */

			sprintf(brdstat.gbl_version_str, 
					"%s Version %s", Version, VERSNUM);

			strncpy(brdstat.gbl_copyright_str,
					Copyright, BRDSTAT_STRLEN);
		
			brdstat.gbl_neqnbrds = nmegaport;
			brdstat.gbl_neqnicps = nicps;
			brdstat.nicps = meg_dev[index].mpd_nicps;
			brdstat.id = meg_dev[index].mpd_id;
			brdstat.bus = meg_dev[index].mpd_bus;
			brdstat.alive = meg_dev[index].mpd_alive;
			brdstat.pmem = (unsigned long) meg_dev[index].mpd_pmem;
			brdstat.addrsz = meg_dev[index].mpd_addrsz;
			brdstat.pgctrl = meg_dev[index].mpd_pgctrl;

			/* update lmx struct */
			for (i=0; i < brdstat.nicps; i++) {
				for (j=0; j < MAXLMX; j++) {
					brdstat.lmx[i][j].lmx_active =
					meg_dev[index].icp[i].lmx[j].lmx_active;
					brdstat.lmx[i][j].lmx_id =
					meg_dev[index].icp[i].lmx[j].lmx_id;
					brdstat.lmx[i][j].lmx_chan =
					meg_dev[index].icp[i].lmx[j].lmx_chan;
					brdstat.lmx[i][j].lmx_rmt_active =
					meg_dev[index].icp[i].lmx[j].lmx_rmt_active;
					brdstat.lmx[i][j].lmx_rmt_id =
					meg_dev[index].icp[i].lmx[j].lmx_rmt_id;
				}
			}

			/* update brd_cst */
			mpc = &meg_chan[index*NCHAN_BRD];
			for (index=0; index < MAXBRDCHNL; index++,mpc++) {
				brd_cst[index].cst_in = mpc->mpc_input;
				brd_cst[index].cst_out = mpc->mpc_output;
			}

			memcpy(&brdstat.brd_cst, brd_cst, sizeof(brd_cst));

			/* copy out the return structure */
			if (eqn_to_user((unsigned char *) arg, &brdstat,
				sizeof(struct eqnbrdstatus))) {
				rc = -EFAULT;
				break;
			}

			break;
			}

      case EQNBRDSTATCLR:
			if (eqn_from_user(&length, (unsigned int *)arg, 
				sizeof(unsigned int))) {
				rc = -EFAULT; 
				break;
			}

			/* Board number is expected to start at 1 (not zero) */
			if(length > nmegaport || length < 1)
				rc = -EINVAL;
			length--; /* make board biased to zero for internal 
					indices */
			mpc = &meg_chan[length*NCHAN_BRD];
			for (length=0; length < MAXBRDCHNL; length++,mpc++) {
				mpc->mpc_input = 0;
				mpc->mpc_output = 0;
				mpc->mpc_parity_err_cnt = 0;
				mpc->mpc_framing_err_cnt = 0;
				mpc->mpc_break_cnt = 0;
			}
			break;

      case EQNCHSTATUS: {
			struct	eqnchstatus	chstat;
			int	index, version;
			struct	slot_struct *slot;

			if (eqn_from_user(&index, (unsigned int *)arg, 
				sizeof(unsigned int))) {
				return(-EFAULT);
			}

			d = SSTMINOR_FROMDEV(index);
			if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
				|| (d < 0)) {
				return(-ENODEV);
			}
        		mpc = &meg_chan[d];
			if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
				return(-ENODEV);
			}
			nchan = MPCHAN(d);
			if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
				nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
			}

			/* verify version */
			if (eqn_from_user(&version, (unsigned int *)arg+1, 
				sizeof(unsigned int))) {
				return(-EFAULT);
			}

			if(version != IOCTL_VERSION) {
				return (-EINVAL);
			}
			
			/*
			** lock mpdev board lock
			*/
			spin_lock_irqsave(&mpd->mpd_lock, flags);

			/* build eqnchstatus structure */

			chstat.ec_nbr = index;
			chstat.ioctl_version = IOCTL_VERSION;

			if (mpc->flags & ASYNC_INITIALIZED){
				chstat.c_iflag = 
					mpc->mpc_tty->termios->c_iflag;
				chstat.c_oflag = 
					mpc->mpc_tty->termios->c_oflag;
				chstat.c_cflag = 
					mpc->mpc_tty->termios->c_cflag;
				chstat.c_lflag = 
					mpc->mpc_tty->termios->c_lflag;
			}
			else {
				chstat.c_iflag = 0;
				chstat.c_oflag = 0; 
				chstat.c_cflag = 0;
				chstat.c_lflag = 0; 
			}

			chstat.ec_flags = mpc->mpc_flags;
			chstat.serial_flags = mpc->flags;
			chstat.ec_openwaitcnt = mpc->openwaitcnt;
			if(ISRAMP(mpc)){
				slot = mpc->mpc_icp->slot_ctrl_blk + 
					((mpc->mpc_chan) % 64);
				chstat.ec_power_state = slot->power_state;
			} else {
				chstat.ec_power_state = 0;
			}
			chstat.ec_mpa_stat = mpc->mpc_mpa_stat;
			
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			/* copy out the return structure */
			if (eqn_to_user((unsigned char *) arg, &chstat,
				sizeof(struct eqnchstatus))) {
				rc = -EFAULT;
				break;
			}

			break;
			}

      case EQNREGISTERS: {
			struct	registers	regs;
			int	index;

			if (eqn_from_user(&index, (unsigned int *)arg, 
				sizeof(unsigned int))) {
				return(-EFAULT);
			}

			d = SSTMINOR_FROMDEV(index);
			if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
				|| (d < 0)) {
				return(-ENODEV);
			}
        		mpc = &meg_chan[d];
			if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
				return(-ENODEV);
			}
			nchan = MPCHAN(d);
			if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
				nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
			}

			/*
			** lock mpdev board lock
			*/
			spin_lock_irqsave(&mpd->mpd_lock, flags);

			/* build registers structure */

			regs.dev = index;

			win16 = mpc->mpc_mpd->mpd_nwin;
			if(win16)
			   mega_push_win(mpc, 0);

			memcpy((unsigned char *) &regs.ec_cin, (unsigned char *) mpc->mpc_icpi,
				sizeof(union input_regs_u));
			memcpy((unsigned char *) &regs.ec_cout, (unsigned char *) mpc->mpc_icpo,
				sizeof(union output_regs_u));

			if(win16)
				mega_pop_win(3);

			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			/* copy out the return structure */
			if (eqn_to_user((unsigned char *) arg, &regs,
				sizeof(struct registers))) {
				rc = -EFAULT;
				break;
			}

			break;
			}

      case EQNCHBUFSTAT: {
			int version;
			struct eqnbufstatus chb,*chbfst = &chb;
			icpiaddr_t icpi;
			icpqaddr_t icpq;
			icpbaddr_t icpb;
			uchar_t c;
			if (eqn_from_user(&chb, (unsigned char *)arg, 
				sizeof(chb))) {
				return(-ENODEV);
			}
			d = SSTMINOR_FROMDEV(chb.ebf_nbr);
			if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
				|| (d < 0)) {
				return(-ENODEV);
			}
        		mpc = &meg_chan[d];
			if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
				return(-ENODEV);
			}
			nchan = MPCHAN(d);
			if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
				nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
			}

			/* verify version */
			if (eqn_from_user(&version, (unsigned int *) arg+1, 
				sizeof(unsigned int))) {
				return (-EFAULT);
			}

			if(version != IOCTL_VERSION) {
				return (-EINVAL);
			}

			/*
			** lock mpdev board lock
			*/
			spin_lock_irqsave(&mpd->mpd_lock, flags);

			win16 = mpc->mpc_mpd->mpd_nwin;
			if(win16)
			   mega_push_win(mpc, 0);

			icpi = mpc->mpc_icpi;

			icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
  			if (!(icpb->ssp.bank_events & EV_REG_UPDT)) 
  				frame_wait(mpc,2); /* make sure regs are valid */ 
			chbfst->ioctl_version = IOCTL_VERSION;
			chbfst->ebf_incount = rx_rcv_cnt;
			icpq = (icpqaddr_t)&mpc->mpc_icpo->ssp.cout_q0;
			chbfst->ebf_outcount = Q_data_count; 
/* above lines mimic this code from streams: (Monty)
			chbfst->ebf_incount = mpc->mpc_rx_bank.ssp.ssp.bank_num_chars; 
			chbfst->ebf_outcount = mpc->mpc_icpo->ssp.cout_q0.q_data_count; 
*/
			if (( mpc->mpc_block)
 				|| (mpc->mpc_cin->ssp.cin_int_flags & 0x40 ))
				chbfst->ebf_inflow_state = 0;
			else
				chbfst->ebf_inflow_state = 1;
			chbfst->ebf_outflow_state = !(mpc->mpc_cin->ssp.cin_int_flags & 0x20);
			c = eqn_cs_get( mpc );
			chbfst->ebf_insigs = c & 0x0f;
			chbfst->ebf_outsigs = (unsigned) (c & 0x30) >> 4;
			if(win16)
				mega_pop_win(4);
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			if (eqn_to_user((unsigned char *) arg, &chb,
				sizeof(chb))) {
				return(-EFAULT);
			}
			} /* end of EQNCHBUFSTAT */
			break;

      case EQNCHLOOPON:
      case EQNCHLOOPOFF:

			if (eqn_from_user(&length, (unsigned int *)arg, 
				sizeof(unsigned int))) {
				return(-EFAULT);
			}

			d = SSTMINOR_FROMDEV(length);
			if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
				|| (d < 0)) {
				return(-ENODEV);
			}
        		mpc = &meg_chan[d];
			if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
				return(-ENODEV);
			}
			nchan = MPCHAN(d);
			if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
				nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
			}

			/*
			** lock mpdev board lock
			*/
			spin_lock_irqsave(&mpd->mpd_lock, flags);

			win16 = mpc->mpc_mpd->mpd_nwin;
			if(win16)
			   mega_push_win(mpc, 0);
  	      		if ( cmd == EQNCHLOOPON ) {
/* RAMP START --------------------------------------------- */
				if (ISRAMP(mpc)) {
              				if (ramp_set_loop_back(mpc)) {
		            			if(win16)
		               				mega_pop_win(46);
						spin_unlock_irqrestore(&mpd->mpd_lock, flags);
						return(-ENXIO);
              				}
		            		if(win16)
		               			mega_pop_win(47);
loopon_wait:
					mpc->mpc_mpa_slb_wait++;
					spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
					/* 2.2 and 2.4 kernels */
					interruptible_sleep_on(
						&mpc->mpc_mpa_slb);
#else
					/* 2.6 kernels */
					wait_event_interruptible(
						mpc->mpc_mpa_slb, 
						mpc->mpc_mpa_slb_wait == 0);
#endif
					spin_lock_irqsave(&mpd->mpd_lock, flags);
					if eqn_fatal_signal {
						spin_unlock_irqrestore(&mpd->mpd_lock, flags);
						return(-ERESTARTSYS);
					}
					if (mpc->mpc_mpa_stat 
						& MPA_SET_LOOP_BACK_ERROR){
						spin_unlock_irqrestore(&mpd->mpd_lock, flags);
						return(-ENXIO);
					}
					else if (mpc->mpc_mpa_stat 
						== MPA_INITG_SET_LOOP_BACK)
						goto loopon_wait;
					if(win16)
			   			mega_push_win(mpc, 0);
				}
				else {
/* RAMP END ----------------------------------------------- */
					/* int/ext loopback, command enable */
					mpc->mpc_cout->ssp.cout_lmx_cmd |= 0x43;
					frame_wait( mpc, 6 );
					/* clear cmd enable */
					mpc->mpc_cout->ssp.cout_lmx_cmd &= 
						~0x40;
				}
				mpc->mpc_flags |= MPC_LOOPBACK;
			}
			else {  /* EQNCHLOOPOFF */
/* RAMP START --------------------------------------------- */
				if (ISRAMP(mpc)) {
              				if (ramp_clr_loop_back(mpc)) {
				     		if(win16)
					     		mega_pop_win(6);
						spin_unlock_irqrestore(&mpd->mpd_lock, flags);
						return(-ENXIO);
			     		}
		        		if(win16)
		           			mega_pop_win(49);
loopoff_wait:
					mpc->mpc_mpa_clb_wait++;
					spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
					/* 2.2 and 2.4 kernels */
					interruptible_sleep_on(
						&mpc->mpc_mpa_clb);
#else
					/* 2.6 kernels */
					wait_event_interruptible(
						mpc->mpc_mpa_clb, 
						mpc->mpc_mpa_clb_wait == 0);
#endif
					spin_lock_irqsave(&mpd->mpd_lock, flags);
					if eqn_fatal_signal {
						spin_unlock_irqrestore(&mpd->mpd_lock, flags);
						return(-ERESTARTSYS);
					}
					if (mpc->mpc_mpa_stat & 
						MPA_CLR_LOOP_BACK_ERROR){
						spin_unlock_irqrestore(&mpd->mpd_lock, flags);
						return(-ENXIO);
              				}
					else if (mpc->mpc_mpa_stat 
						== MPA_INITG_SET_LOOP_BACK)
						goto loopoff_wait;
					if(win16)
			   			mega_push_win(mpc, 0);
				} else {
/* RAMP END ----------------------------------------------- */
					mpc->mpc_cout->ssp.cout_lmx_cmd &= 
						~0x03;   /* int/ext loopback */
					mpc->mpc_cout->ssp.cout_lmx_cmd 
						|= 0x40;   /* command enable */
					frame_wait( mpc, 6 );
					/* clear cmd enable */
					mpc->mpc_cout->ssp.cout_lmx_cmd 
						&= ~0x40;
				}
				mpc->mpc_flags &= ~MPC_LOOPBACK;
			}
			if(win16)
				mega_pop_win(6);
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			break;

	case EQNCHRESET:
	{
		icpiaddr_t icpi;
		icpbaddr_t icpb;

		if (eqn_from_user(&length, (unsigned int *)arg, 
			sizeof(unsigned int))) {
			 return(-EFAULT); 
		}
			
		d = SSTMINOR_FROMDEV(length);
		if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
			|| (d < 0)) {
			return(-ENODEV);
		}
#ifdef DEBUG
		printk("EQNCHRESET: d = %d", d);
#endif
        	mpc = &meg_chan[d];
		if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
			return(-ENODEV);
		}
		nchan = MPCHAN(d);
		if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
			nchan >= (int)mpd->mpd_nchan){
			return(-ENODEV);
		}

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpd->mpd_lock, flags);

		/* PatH - Multimodem  reset */
		if (mpd->mpd_board_def->flags & MM)
		{
  			icpi = mpc->mpc_icpi;
			icpb = (rx_locks & LOCK_A) ? 
				&icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
			GET_CTRL_SIGS(mpc, cur);
			/* Turn on reset signal */
			cur |= TX_HFC_2 | TX_CNT_2;
			if((rx_ctrl_sig & 0x2020) == 0x2020)
				cur |= 0x8000;
	  		else
			 	cur &= ~0x8000;
	  		cur ^= TX_SND_CTRL_TG;
			SET_CTRL_SIGS(mpc, cur);
			/* Need to pause for 150 ms */
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			eqnx_delay( HZ*15 / 100);
			spin_lock_irqsave(&mpd->mpd_lock, flags);
			/* Turn off reset signal */
	  		cur &= ~(TX_HFC_2 | TX_CNT_2);
	  		cur ^= TX_SND_CTRL_TG;
			SET_CTRL_SIGS(mpc, cur);
		}	

/* RAMP START --------------------------------------------- */
		if (!(ISRAMP(mpc))){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			return(0);
		}
		/* If modem not registered, don't call reset */
		if (!(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE)){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			return(0);
		}
		win16 = mpc->mpc_mpd->mpd_nwin;
		if(win16)
		   mega_push_win(mpc, 0);
		ramp_hard_reset(mpc);
		if(win16)
			mega_pop_win(61);
reset_wait:
		mpc->mpc_mpa_rst_wait++;
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		interruptible_sleep_on(&mpc->mpc_mpa_rst);
#else
		/* 2.6 kernels */
		wait_event_interruptible(mpc->mpc_mpa_rst, 
			mpc->mpc_mpa_rst_wait == 0);
#endif
		spin_lock_irqsave(&mpd->mpd_lock, flags);
		if eqn_fatal_signal {
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
			return(-ERESTARTSYS);
		}
		if (mpc->mpc_mpa_stat & MPA_HARD_RESET_ERROR){
			spin_unlock_irqrestore(&mpd->mpd_lock, flags);
#ifdef DEBUG
printk("hard_reset_error for device %d\n", d);
#endif
			return(-ENXIO);
		}
		else if (mpc->mpc_mpa_stat 
				== MPA_INITG_HARD_RESET)
			goto reset_wait;
/* RAMP END ----------------------------------------------- */
	}
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		break;
	default:
		rc = -ENOIOCTLCMD;
		break;
	}
	break;
	case 1:
	case 2:
	switch (cmd){
	case EQNCHMONSET:
	case EQNCHMONCLR:{
		int d;
		if (eqn_from_user(&length, (unsigned int *)arg, 
				sizeof(unsigned int))) {
			 return(-EFAULT); 
		}
			
		d = SSTMINOR_FROMDEV(length);
		if((d > nmegaport*NCHAN_BRD) || (nmegaport == 0)
			|| (d < 0)) {
			return(-ENODEV);
		}
        	mpc = &meg_chan[d];
		if ((mpd = mpc->mpc_mpd) == (struct mpdev *) NULL){
			return(-ENODEV);
		}
		nchan = MPCHAN(d);
		if (mpd >= &meg_dev[nmegaport] || mpd->mpd_alive == 0 ||
			nchan >= (int)mpd->mpd_nchan){
				return(-ENODEV);
		}

		/*
		** lock mpdev board lock
		*/
		spin_lock_irqsave(&mpd->mpd_lock, flags);

		if (cmd == EQNCHMONSET){
			dscope[minor -1].chan = d;
			if (minor & 1)
				mpc->mpc_flags |= MPC_DSCOPER;
			else
				mpc->mpc_flags |= MPC_DSCOPEW;
		}
		else{
			dscope[minor - 1].chan = -1;
			if (minor & 1)
				mpc->mpc_flags &= ~MPC_DSCOPER;
			else
				mpc->mpc_flags &= ~MPC_DSCOPEW;
		}
		}
		spin_unlock_irqrestore(&mpd->mpd_lock, flags);
		break;
	default:
		rc = -ENOIOCTLCMD;
		break;
	}
	}
#ifdef DEBUG
	printk("eqnx_diagioctl: ret val %d for cmd %x for device %d\n", 
		rc, (unsigned int) cmd, MINOR(ip->i_rdev));
#endif
	return(rc);
}

/*
** eqnx_diagread(fp, buf, count, fpos)
**
*/
/* Pat */
#if (EQNX_VERSION_CODE > 131327)
static ssize_t eqnx_diagread(struct file *fp, char *buf, size_t count, loff_t *fpos)
#else		
static int eqnx_diagread(struct inode *ip, struct file *fp, char *buf, 
		int count)
#endif
{
	int d, cc,n, ccm,rc;
#if (EQNX_VERSION_CODE > 131327)
	struct dentry *dent;
	struct inode *ip;
	
	dent = fp->f_dentry;
	ip = dent->d_inode;
#endif
	d = MINOR(ip->i_rdev) - 1;
	if ((d != 0) && (d != 1))
		return(-ENODEV);
	cc = (dscope[d].next - dscope[d].q.q_ptr) & (DSQMASK);
	if (!(cc)){
		if (fp->f_flags & O_NONBLOCK) {
			return(0);
		}
		dscope[d].scope_wait_wait++;
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		interruptible_sleep_on(&dscope[d].scope_wait);
#else
		/* 2.6 kernels */
		wait_event_interruptible(dscope[d].scope_wait, 
			dscope[d].scope_wait_wait == 0);
#endif
		cc = (dscope[d].next - dscope[d].q.q_ptr) & (DSQMASK);
	}
	rc = ccm = MIN(cc, count);
	while (ccm > 0) {
		n= MIN(ccm,dscope[d].q.q_end - dscope[d].q.q_ptr + 1);
                if (eqn_to_user(buf, (unsigned char *) (dscope[d].q.q_addr 
			+ dscope[d].q.q_ptr), (sizeof(char) * n)))
			return (-EFAULT);
                buf += n;
                ccm -= n;
                dscope[d].q.q_ptr += n;
                if (dscope[d].q.q_ptr > dscope[d].q.q_end)
                        dscope[d].q.q_ptr = dscope[d].q.q_begin;
	}
	return(rc);
}

/*
** eqnx_diagopen(ip, fp)
**
*/
STATIC
int eqnx_diagopen(struct inode *ip, struct file *fp)
{
	int d;

	d = MINOR(ip->i_rdev);
	if (!d)
	{
#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
		MOD_INC_USE_COUNT;
#else
	/* 2.6 kernels */
		try_module_get(THIS_MODULE);
#endif
		return(0);
	}
	d--;
	if ((d != 0) && (d != 1))
		return(-ENODEV);
	if (dscope[d].open)
		return(-EBUSY);

	dscope[d].q.q_addr = dscope[d].buffer;
	dscope[d].q.q_begin = 0;
	dscope[d].q.q_end = DSQMASK;
	dscope[d].q.q_ptr = dscope[d].q.q_begin;
	dscope[d].next = dscope[d].q.q_begin;
	dscope[d].open = true;
	dscope[d].chan = -1;

#if	(LINUX_VERSION_CODE < 132096)
	/* kernels before 2.4 */
	init_waitqueue(&dscope[d].scope_wait);
#else
	/* 2.4 kernels and after */
	dscope[d].scope_wait_wait = 0;
	init_waitqueue_head(&dscope[d].scope_wait);
#endif

#if	(LINUX_VERSION_CODE < 132608)
	/* 2.2 and 2.4 kernels */
	MOD_INC_USE_COUNT;
#else
	/* 2.6 kernels */
	try_module_get(THIS_MODULE);
#endif
	return(0);
}

/*
** eqnx_diagclose(ip, fp)
**
*/
STATIC
int eqnx_diagclose(struct inode *ip, struct file *fp)
{
	int d;

	/* 2.6 kernels */
	module_put(THIS_MODULE);
	d = MINOR(ip->i_rdev);
	if (!d)
		return (0);
	d--;
	if ((d != 0) && (d != 1))
		return (0);
	dscope[d].chan = -1;
	dscope[d].open = false;

	return(0);
}

/*
** SSTMINOR(maj, min)
**
*/
static int SSTMINOR(unsigned int maj, unsigned int min)
{ 
	int i;
	if ((maj < din_num[0]) || (maj > diag_num)) {
		return(-1);
	}
		for ( i=0; i < (maxbrd /2); i++)
			if (maj == din_num[i])
				break;
	return((i * 256) + min);
}

/*
** SSTMINOR_FROMDEV(dev)
**
*/
static int SSTMINOR_FROMDEV(unsigned int dev)
{ 
	return SSTMINOR((dev >> 8), (dev & 0xff));
}

/*
** sst_write(mpc, buf, count)
**
** mpdev board lock ** MUST ** be held			 
*/
static void sst_write( struct mpchan *mpc, unsigned char *buf, int count)
{
	int c, nx, win16, datascope = 0;
	volatile icpoaddr_t icpo;
	volatile icpgaddr_t icpg;
	volatile icpqaddr_t icpq;
	unsigned char oldreg;
#ifdef MIDNIGHT
        unsigned char *dst_addr;
        int align, bytes;
#endif

#ifdef	DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in sst_write()\n");
	}
#endif	/* DEBUG_LOCKS */

	icpo = mpc->mpc_icpo;
	icpg = (icpgaddr_t)icpo;
	icpq = &icpo->ssp.cout_q0;
	win16 = mpc->mpc_mpd->mpd_nwin;

		c = count;
		mpc->mpc_flags |= MPC_BUSY;
		if ((mpc->mpc_flags & MPC_DSCOPEW)
			&& (dscope[1].chan == mpc - meg_chan)) datascope=1;
		/*
	 	* Blindingly fast block copy direct from 
	 	* driver buffer to on-board buffers.
	 	*/

		if(win16)
			mega_push_win(mpc, 2);
		nx = MIN(c, (mpc->mpc_txq.q_end - mpc->mpc_txq.q_ptr +1));
#ifdef MIDNIGHT
                dst_addr = mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr;
                align = ((unsigned long)dst_addr) % 4;
                if (align) {
                  bytes = MIN(nx,(4-align));
                  memcpy(dst_addr, buf, bytes);
                }
                else {
                  bytes = 0;
                }
                if (nx > bytes) {
                  memcpy(dst_addr+bytes, buf+bytes, nx-bytes);
                }
#else
		memcpy((mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr), 
			buf, nx);
#endif
		mpc->mpc_txq.q_ptr += nx;
		if(mpc->mpc_txq.q_ptr > mpc->mpc_txq.q_end)
			mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
		if (datascope) {
			int room,move_cnt;
			int d=1;
			char *ubase = buf;
			room = DSQSIZE - ((dscope[d].next - dscope[d].q.q_ptr) 
				& (DSQMASK));
			if (nx > room)
				dscope[d].status |= DSQWRAP;
                	else
                        	room = nx;
			while (room > 0) {
				move_cnt = MIN(room,
					dscope[d].q.q_end - dscope[d].next + 1);
				memcpy(dscope[d].q.q_addr + dscope[d].next, 
					ubase, move_cnt);
                        	dscope[d].next += move_cnt;
                        	if (dscope[d].next > dscope[d].q.q_end)
                                	dscope[d].next = dscope[d].q.q_begin;
                                ubase += move_cnt;
                                room -= move_cnt;
			} /* while room */
			dscope[d].scope_wait_wait = 0;
			wake_up_interruptible(&dscope[d].scope_wait);
		} /* if MPC_DSCOPE */
		buf += nx;
		count -= nx;
		c -= nx;
		if( c ) {
#ifdef MIDNIGHT
                  dst_addr = mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr;
                  align = ((unsigned long)dst_addr) % 4;
                  if (align) {
                        bytes = MIN(c,(4-align));
                        memcpy(dst_addr, buf, bytes);
                  }
                  else {
                        bytes = 0;
                  }
                  if (c > bytes) {
                        memcpy(dst_addr+bytes, buf+bytes, c-bytes);
                  }
#else
			memcpy(mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr, 
				buf, c);
#endif
			mpc->mpc_txq.q_ptr += c;
			if(mpc->mpc_txq.q_ptr > mpc->mpc_txq.q_end)
				mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
			if (datascope) {
				int room,move_cnt;
				int d=1;
				char *ubase = buf;
				room = DSQSIZE - ((dscope[d].next - 
					dscope[d].q.q_ptr) & (DSQMASK));
				if (c > room)
					dscope[d].status |= DSQWRAP;
	                	else
	                        	room = c;
				while (room > 0) {
					move_cnt = MIN(room,dscope[d].q.q_end - 
						dscope[d].next + 1);
					memcpy(dscope[d].q.q_addr + 
						dscope[d].next, ubase,move_cnt);
	                        	dscope[d].next += move_cnt;
	                        	if (dscope[d].next > dscope[d].q.q_end)
	                                	dscope[d].next = 
							dscope[d].q.q_begin;
	                                ubase += move_cnt;
	                                room -= move_cnt;
				} /* while room */
				dscope[d].scope_wait_wait = 0;
				wake_up_interruptible(&dscope[d].scope_wait);
			} /* if MPC_DSCOPE */
			buf += c;
			count -= c;
		}
		if(win16)
			mega_pop_win(24);
	if(win16)
		mega_push_win(mpc, 0);
		oldreg = tx_cpu_reg;
		tx_cpu_reg |= TX_SUSP;
		cur_chnl_sync(mpc);
		if(Q_data_count < 9) {
			if ((icpo->ssp.cout_int_save_togl & 0x4) == 
				(icpo->ssp.cout_cpu_req & 0x4)){
				tx_cpu_reg ^= (CPU_SND_REQ);
				mpc->mpc_cout_events &= ~EV_CPU_REQ_DN;
			}
		}
		Q_data_count += (c + nx);
		/*space -= (c + nx);*/
#ifdef DEBUG
		printk("sst_write: wrote %d chars for device %d\n", (c + nx), 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
		if (!(oldreg & TX_SUSP))
			tx_cpu_reg &= ~TX_SUSP;
		mpc->mpc_output += (c + nx);
		tx_cie |= (ENA_TX_EMPTY_Q0| ENA_TX_LOW_Q0);
	/*}*/
	if(win16)
		mega_pop_win(25);
}

/*
** register_eqnx_dev
**
*/
int register_eqnx_dev(void)
{
	int i,j;
	int num_drivers;

	num_drivers = MAX(1, (((nmegaport - 1)/2) + 1));

	/* Initialize the tty_driver structure */
#ifdef DEBUG
	printk("EQNX: registering the driver\n");
#endif
	if((eqnx_driver =
			(struct tty_driver *)vmalloc(sizeof(struct tty_driver) 
			* num_drivers)) 
			== (struct tty_driver *) NULL){
		printk("EQUINOX: Failed to allocate virtual address space of size %d for eqnx_driver\n", (unsigned int)(sizeof(struct tty_driver) * num_drivers));
		return(-1);
	}
	memset(eqnx_driver, 0, (sizeof(struct tty_driver) 
			* num_drivers));

	if((eqnx_ttys = 
		(struct tty_struct **)vmalloc(sizeof(struct tty_struct *) 
			* nmegaport * NCHAN_BRD)) 
			== (struct tty_struct **) NULL){
		printk("EQUINOX: Failed to allocate virtual address space of size %d for eqnx_ttys\n", (unsigned int)(sizeof(struct tty_struct *) * nmegaport * NCHAN_BRD));
		return(-1);
	}
	memset(eqnx_ttys, 0, (sizeof(struct tty_struct *) 
			* nmegaport * NCHAN_BRD));
	if((eqnx_termios = 
		(struct termios **)vmalloc(sizeof(struct termios *) 
			* nmegaport * NCHAN_BRD)) == (struct termios **) NULL){
		printk("EQUINOX: Failed to allocate virtual address space of size %d for eqnx_termios\n", (unsigned int)(sizeof(struct termios *) * nmegaport * NCHAN_BRD));
		return(-1);
	}

	memset(eqnx_termios, 0, (sizeof(struct termios *) 
			* nmegaport * NCHAN_BRD));
	
	if((eqnx_termioslocked = 
		(struct termios **)vmalloc(sizeof(struct termios *) 
			* nmegaport * NCHAN_BRD)) == (struct termios **) NULL){
		printk("EQUINOX: Failed to allocate virtual address space of size %d for eqnx_termioslocked\n", 
		(unsigned int)(sizeof(struct termios *) * nmegaport * NCHAN_BRD));
		return(-1);
	}
	memset(eqnx_termioslocked, 0, (sizeof(struct termios *) 
			* nmegaport * NCHAN_BRD));
	for (i =0, j = 0; i < nmegaport; i += 2, j++){
		memset(&eqnx_driver[j], 0, sizeof(struct tty_driver));
			eqnx_driver[j].magic 
			= TTY_DRIVER_MAGIC;
		eqnx_driver[j].name = "Eqnx tty";
#ifdef	CCM48
		eqnx_driver[j].major = 253;
#else
		eqnx_driver[j].major = 0;
#endif

			eqnx_driver[j].minor_start = 0;
		if (nmegaport >= (i + 2)){
				eqnx_driver[j].num = 256;
		}else{
				eqnx_driver[j].num = 128;
		}
			eqnx_driver[j].type = TTY_DRIVER_TYPE_SERIAL;
		eqnx_driver[j].subtype = SERIAL_TYPE_NORMAL;
			eqnx_driver[j].init_termios = eqnx_deftermios;
			eqnx_driver[j].flags = TTY_DRIVER_REAL_RAW |
				TTY_DRIVER_NO_DEVFS;
			eqnx_driver[j].termios = 
			&eqnx_termios[j * 256];
			eqnx_driver[j].termios_locked = 
				&eqnx_termioslocked[j * 256];
			eqnx_driver[j].open = eqnx_open;
			eqnx_driver[j].close = eqnx_close;
			eqnx_driver[j].write = eqnx_write;
			eqnx_driver[j].put_char = eqnx_put_char;
			eqnx_driver[j].flush_chars = eqnx_flush_chars;
			eqnx_driver[j].write_room = eqnx_write_room;
			eqnx_driver[j].chars_in_buffer = eqnx_chars_in_buffer;
			eqnx_driver[j].flush_buffer = eqnx_flush_buffer;
			eqnx_driver[j].ioctl = eqnx_ioctl;
			eqnx_driver[j].throttle = eqnx_throttle;
			eqnx_driver[j].unthrottle = eqnx_unthrottle;
			eqnx_driver[j].set_termios = eqnx_set_termios;
			eqnx_driver[j].stop = eqnx_stop;
			eqnx_driver[j].start = eqnx_start;
			eqnx_driver[j].hangup = eqnx_hangup;
#if	(LINUX_VERSION_CODE >= 132608)
		/* 2.6+ kernels */
		eqnx_driver[j].tiocmget = eqnx_tiocmget;
		eqnx_driver[j].tiocmset = eqnx_tiocmset;
#endif
	
#if (LINUX_VERSION_CODE < 132096)
		/*
		 * The callout device is just like normal device except for
		 * major number and the subtype code.
		 */
		eqnx_callout_driver[j].name = "Eqnx cu";
#ifdef	CCM48
		eqnx_callout_driver[j].major = 253;
#else
		eqnx_callout_driver[j].major = 0;
#endif

		eqnx_callout_driver[j].subtype = SERIAL_TYPE_CALLOUT;
#endif
	
	}
	/* Register ALL callout devices before registering normal devices */
	/* Order is reversed; kernel assigns major nums "top down" */
	/* and we end on first board so that din_num, dout_num is valid */
	j--;
#if (LINUX_VERSION_CODE < 132096)
	for ( i=j ; i >= 0 ; i--){
		if ((dout_num[i] = tty_register_driver(&eqnx_callout_driver[i])) <= 0)
		   printk("Couldn't register eqnx_callout_driver for driver %d, error = %d\n", i, dout_num[i]);
	}
#endif
	/* Now register normal devices */
	for ( i=j ; i >= 0 ; i--){
#if	(LINUX_VERSION_CODE < 132608)
		/* 2.2 and 2.4 kernels */
		din_num[i] = tty_register_driver(&eqnx_driver[i]);
#else
		if (tty_register_driver(&eqnx_driver[i])) {
			printk("tty_register_driver failed\n");
			continue;
		}
		din_num[i] = eqnx_driver[i].major;
#endif
		if (din_num[i] <= 0)
			printk("Couldn't register eqnx_driver for driver %d, error = %d\n", i,din_num[i]);

		else 
			ist.ctlr_array[2*i].ctlr_major = ist.ctlr_array[2*i + 1].ctlr_major = din_num[i];
		
	}
	return(0);
}

/*
** sst_write1(mpc, func_type)
**
** mpdev board lock ** MUST ** be held			 
*/
static void sst_write1(struct mpchan *mpc, int func_type)
{
	struct tty_struct *tty = mpc->mpc_tty;
	int space, c, nx, win16, datascope = 0;
	volatile icpoaddr_t icpo;
	volatile icpgaddr_t icpg;
	volatile icpqaddr_t icpq;
	unsigned char oldreg;
	int block_count;
#ifdef MIDNIGHT
        unsigned char *dst_addr;
        int align, bytes;
#endif

#ifdef DEBUG
	printk("icpbaud: device %d\n", mpc->mpc_chan);
#ifdef DEBUG_LOCKS
	if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in sst_write1()\n");
	}
#endif	/* DEBUG_LOCKS */
#endif	/* DEBUG */

	icpo = mpc->mpc_icpo;
	icpg = (icpgaddr_t)icpo;
	icpq = &icpo->ssp.cout_q0;
	win16 = mpc->mpc_mpd->mpd_nwin;

	if(win16)
		mega_push_win(mpc, 0);
	block_count = icpq->ssp.q_block_count * 64;
	space = mpc->mpc_txq.q_size - 
		(mpc->mpc_count + Q_data_count) -1;
#ifdef DEBUG
	printk("sst_write1: device = %d, q_size = %d, space = %d, xmit_cnt = %d, xmit_tail = %d, xmit_head = %d\n",
	SSTMINOR(mpc->mpc_major, mpc->mpc_minor),mpc->mpc_txq.q_size,space, mpc->xmit_cnt, mpc->xmit_tail, mpc->xmit_head);
#endif
	if ((func_type != EQNX_TXINT) && 
		((space < block_count) && (mpc->xmit_cnt >= block_count))){
			if(win16)
			mega_pop_win(25);
		return;
	}
	while(1){
		c = MIN(space, MIN(mpc->xmit_cnt,
				XMIT_BUF_SIZE - mpc->xmit_tail));
		if ( c <= 0 )
			break;
#ifdef DEBUG
		printk("sst_write1: device = %d, xmit_cnt = %d\n", 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor), mpc->xmit_cnt);
#endif
		mpc->mpc_flags |= MPC_BUSY;
		if ((mpc->mpc_flags & MPC_DSCOPEW)
			&& (dscope[1].chan == mpc - meg_chan)) datascope=1;
		/*
	 	* Blindingly fast block copy direct from 
	 	* driver buffer to on-board buffers.
	 	*/

		if (mpc->xmit_buf == NULL)
			break;

		if(win16)
			mega_push_win(mpc, 2);
		nx = MIN(c, (mpc->mpc_txq.q_end - mpc->mpc_txq.q_ptr +1));
#ifdef MIDNIGHT
                dst_addr = mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr;
                align = ((unsigned long)dst_addr) % 4;
                if (align) {
                  bytes = MIN(nx,(4-align));
                  memcpy(dst_addr, mpc->xmit_buf + mpc->xmit_tail , bytes);
                }
                else {
                  bytes = 0;
                }
                if (nx > bytes) {
                  memcpy(dst_addr+bytes, 
			mpc->xmit_buf + mpc->xmit_tail + bytes, nx-bytes);
                }
#else
		memcpy((mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr), 
			mpc->xmit_buf + mpc->xmit_tail, nx);
#endif
		mpc->mpc_txq.q_ptr += nx;
		if(mpc->mpc_txq.q_ptr > mpc->mpc_txq.q_end)
			mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
		if (datascope) {
			int room,move_cnt;
			int d=1;
			char *ubase = mpc->xmit_buf + mpc->xmit_tail;
			room = DSQSIZE - ((dscope[d].next - dscope[d].q.q_ptr) 
				& (DSQMASK));
			if (nx > room)
				dscope[d].status |= DSQWRAP;
                	else
                        	room = nx;
			while (room > 0) {
				move_cnt = MIN(room,
					dscope[d].q.q_end - dscope[d].next + 1);
				memcpy(dscope[d].q.q_addr + dscope[d].next, 
					ubase, move_cnt);
                        	dscope[d].next += move_cnt;
                        	if (dscope[d].next > dscope[d].q.q_end)
                                	dscope[d].next = dscope[d].q.q_begin;
                                ubase += move_cnt;
                                room -= move_cnt;
			} /* while room */
			dscope[d].scope_wait_wait = 0;
			wake_up_interruptible(&dscope[d].scope_wait);
		} /* if MPC_DSCOPE */
		mpc->xmit_tail += nx;
		mpc->xmit_tail &= XMIT_BUF_SIZE - 1;
		mpc->xmit_cnt -= nx;
		c -= nx;
		if (mpc->xmit_buf == NULL)
			break;

		if( c ) {
#ifdef MIDNIGHT
                  dst_addr = mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr;
                  align = ((unsigned long)dst_addr) % 4;
                  if (align) {
                        bytes = MIN(c,(4-align));
                        memcpy(dst_addr, mpc->xmit_buf + mpc->xmit_tail, bytes);
                  }
                  else {
                        bytes = 0;
                  }
                  if (c > bytes) {
                        memcpy(dst_addr+bytes, 
				mpc->xmit_buf + mpc->xmit_tail +bytes, c-bytes);
                  }
#else
			memcpy(mpc->mpc_txq.q_addr + mpc->mpc_txq.q_ptr, 
				mpc->xmit_buf + mpc->xmit_tail, c);
#endif
			mpc->mpc_txq.q_ptr += c;
			if(mpc->mpc_txq.q_ptr > mpc->mpc_txq.q_end)
				mpc->mpc_txq.q_ptr = mpc->mpc_txq.q_begin;
			if (datascope) {
				int room,move_cnt;
				int d=1;
				char *ubase = mpc->xmit_buf + mpc->xmit_tail;
				room = DSQSIZE - ((dscope[d].next - 
					dscope[d].q.q_ptr) & (DSQMASK));
				if (c > room)
					dscope[d].status |= DSQWRAP;
	                	else
	                        	room = c;
				while (room > 0) {
					move_cnt = MIN(room,dscope[d].q.q_end - 
						dscope[d].next + 1);
					memcpy(dscope[d].q.q_addr + 
						dscope[d].next, ubase,move_cnt);
	                        	dscope[d].next += move_cnt;
	                        	if (dscope[d].next > dscope[d].q.q_end)
	                                	dscope[d].next = 
							dscope[d].q.q_begin;
	                                ubase += move_cnt;
	                                room -= move_cnt;
				} /* while room */
				dscope[d].scope_wait_wait = 0;
				wake_up_interruptible(&dscope[d].scope_wait);
			} /* if MPC_DSCOPE */
			mpc->xmit_tail += c;
			mpc->xmit_tail &= XMIT_BUF_SIZE - 1;
			mpc->xmit_cnt -= c;
		}
		if(win16)
			mega_pop_win(24);
		oldreg = tx_cpu_reg;
		tx_cpu_reg |= TX_SUSP;
		cur_chnl_sync(mpc);
		if(Q_data_count < 9) {
			if ((icpo->ssp.cout_int_save_togl & 0x4) == 
				(icpo->ssp.cout_cpu_req & 0x4)){
				tx_cpu_reg ^= (CPU_SND_REQ);
				mpc->mpc_cout_events &= ~EV_CPU_REQ_DN;
			}
		}
		Q_data_count += (c + nx);
		space -= (c + nx);
#ifdef DEBUG
		printk("sst_write1: wrote %d chars for device %d\n", (c + nx), 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
		printk("q data count %d, space %d for device %d\n", 
			Q_data_count,space, SSTMINOR(mpc->mpc_major,
				mpc->mpc_minor));
		printk("xmit_cnt = %d, xmit_tail = %d, xmit_head = %d\n",
			mpc->xmit_cnt, mpc->xmit_tail, mpc->xmit_head);
#endif
		if (!(oldreg & TX_SUSP))
			tx_cpu_reg &= ~TX_SUSP;
		mpc->mpc_output += (c + nx);
		tx_cie |= (ENA_TX_EMPTY_Q0| ENA_TX_LOW_Q0);
	}
	if ((mpc->xmit_cnt < block_count) && (func_type == EQNX_TXINT)){
#ifdef DEBUG
		printk("sst_write1: calling write_wakeup for device = %d\n", 
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
		/* signal write wakeup when safe to call ldisc */
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) && tty->ldisc.write_wakeup)
			write_wakeup_deferred++;
		wake_up_interruptible(&tty->write_wait);
	}
	if(win16)
		mega_pop_win(25);
}

/*
** ramp_map_fn(mpc_arg)
**
*/
void *ramp_map_fn( void *mpc_arg)
{
	unsigned long flags;
	struct mpchan *mpc = (struct mpchan *) mpc_arg;

	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	mega_push_win((struct mpchan *)mpc,0);
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
   	return(mpc);
}

/*
** ramp_unmap_fn(mpc)
**
*/
void ramp_unmap_fn(void *mpc_arg)
{
	unsigned long flags;
	struct mpchan *mpc = (struct mpchan *) mpc_arg;

	spin_lock_irqsave(&mpc->mpc_mpd->mpd_lock, flags);
	mega_pop_win(57); 
	spin_unlock_irqrestore(&mpc->mpc_mpd->mpd_lock, flags);
}

/*
** ramp_id_functs()
**
** Provide the addresses of functions to block interrupts, and to
** unblock interrupts for use by the ramp services code.
** The driver does not use locking, so the addresses of the
** locking routines are initialized to NULL.
**
*/
static int ramp_id_functs(void)
{
   int ret = 0;
   int retv = 0;

   ramp_import_blk.blk_ints_fn = ramp_block_int;
   ramp_import_blk.rstr_ints_fn = ramp_unblock_int;
   retv = mpa_srvc_id_functs(&ramp_import_blk);
   if (retv == 0x00FC) {
      struct mpchan *mpc;
      int i;

      for(i = 0, mpc = meg_chan; i < nmegaport * NCHAN_BRD; i++, mpc++)
         mpc->mpc_mpa_stat = MPA_INIT_ERROR;
      ret = retv;
   } else
      ret = 0;
   return(ret); 
}

/*
** ramp_block_int()
**
** blocks interrupt.
*/
static unsigned long int ramp_block_int(void)
{
	unsigned long flags;

	spin_lock_irqsave(&ramp_lock, flags);
	return(flags);
}

/*
** ramp_unblock_int(flags)
**
** unblocks interrupt.
*/
static void ramp_unblock_int(unsigned long int flags)
{
	spin_unlock_irqrestore(&ramp_lock, flags);
}

/*
** ramp_check_error(mpc, retv)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_check_error( struct mpchan *mpc, int retv)
{

#ifdef	DEBUG_LOCKS
   if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in ramp_check_error()\n");
   }
#endif	/* DEBUG_LOCKS */
#ifdef DEBUG_RAMP
printk("ramp_check_error:chan %d with stat = %d\n", mpc->mpc_chan,
			mpc->mpc_mpa_stat);
#endif
   if (mpc->mpc_mpa_stat & MPA_INITG_UART) {
      ramp_reg_modem_error(mpc, retv);
   }
   if (mpc->mpc_mpa_stat & MPA_INITG_SET_LOOP_BACK) {
      ramp_set_loop_back_error(mpc, retv);
   }
   if (mpc->mpc_mpa_stat & MPA_INITG_CLR_LOOP_BACK) {
      ramp_clr_loop_back_error(mpc, retv);
   }
   if (mpc->mpc_mpa_stat & MPA_INITG_START_BREAK) {
      ramp_start_break_error(mpc, retv);
   }
   if (mpc->mpc_mpa_stat & MPA_INITG_STOP_BREAK) {
      ramp_stop_break_error(mpc, retv);
   }
   if (mpc->mpc_mpa_stat & MPA_INITG_HARD_RESET) {
      ramp_hard_reset_error(mpc, retv);
   }
   if ((mpc->mpc_mpa_stat & MPA_INITG_INIT_MODEM) ||
      (mpc->mpc_mpa_stat & MPA_INITG_MODIFY_SETTINGS)) {
      ramp_init_modem_error(mpc, retv);
   }
}

/*
** ramp_reg_modem_error(mpc, retv)
**
** Sets mpa_stat flag to indicate error during register modem,
** prints error message on console, and wakes up any sleepers.
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_reg_modem_error( struct mpchan *mpc, int retv)
{
   mpc->mpc_mpa_stat &= ~MPA_INITG_UART; 
   mpc->mpc_mpa_stat |= MPA_INIT_ERROR;  
   if (mpc->mpc_mpa_cout_ctrl_sigs) {
      SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
      mpc->mpc_mpa_cout_ctrl_sigs = 0;
   }
MESSAGE("ERROR: reg_modem", retv, mpc);
      mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_CLR_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;
      mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
   mpc->mpc_mpa_slb_wait = 0;
   mpc->mpc_mpa_clb_wait = 0;
   mpc->mpc_mpa_rst_wait = 0;
   mpc->mpc_mpa_call_back_wait = 0;
   wake_up_interruptible(&mpc->mpc_mpa_slb);
   wake_up_interruptible(&mpc->mpc_mpa_clb);
   wake_up_interruptible(&mpc->mpc_mpa_rst);
   wake_up_interruptible(&mpc->mpc_mpa_call_back);
}

/*
** ramp_set_loop_back_error(mpc, retv)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_set_loop_back_error( struct mpchan *mpc, int retv)
{
   mpc->mpc_mpa_stat &= ~MPA_INITG_SET_LOOP_BACK;
   mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_CLR_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;
      mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
   mpc->mpc_mpa_slb_wait = 0;
   mpc->mpc_mpa_clb_wait = 0;
   mpc->mpc_mpa_rst_wait = 0;
   mpc->mpc_mpa_call_back_wait = 0;
   wake_up_interruptible(&mpc->mpc_mpa_slb);
   wake_up_interruptible(&mpc->mpc_mpa_clb);
   wake_up_interruptible(&mpc->mpc_mpa_rst);
   wake_up_interruptible(&mpc->mpc_mpa_call_back);
   MESSAGE("ERROR: set_loop_back", retv, mpc);
}

/*
** ramp_clr_loop_back_error(mpc, retv)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_clr_loop_back_error( struct mpchan *mpc, int retv)
{
   mpc->mpc_mpa_stat &= ~MPA_INITG_CLR_LOOP_BACK;
   mpc->mpc_mpa_stat &= ~MPA_SET_LOOP_BACK_DONE;
   mpc->mpc_mpa_stat |= MPA_CLR_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;
	mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
   mpc->mpc_mpa_slb_wait = 0;
   mpc->mpc_mpa_clb_wait = 0;
   mpc->mpc_mpa_rst_wait = 0;
   mpc->mpc_mpa_call_back_wait = 0;
   wake_up_interruptible(&mpc->mpc_mpa_clb);
   wake_up_interruptible(&mpc->mpc_mpa_slb);
   wake_up_interruptible(&mpc->mpc_mpa_rst);
   wake_up_interruptible(&mpc->mpc_mpa_call_back);
   MESSAGE("ERROR: clr_loop_back", retv, mpc);
}

/*
** ramp_clr_loop_back(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
int ramp_clr_loop_back( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int slot_chan, port, lmx, retv, ret = 0;
   unsigned int state = mpc->mpc_mpa_stat;

#ifdef	DEBUG_LOCKS
   if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
		printk("LOCK Failure: mpd board lock NOT locked in ramp_clr_loop_back()\n");
   }
#endif	/* DEBUG_LOCKS */

   /* If doing non-waited call, try again later. */
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~MPA_INITG_CLR_LOOP_BACK)) {
      if (mpc->mpc_mpa_delay[CLR_LOOP_BACK_INDEX] == 0)
         mpc->mpc_mpa_delay[CLR_LOOP_BACK_INDEX] = ramp_get_delay(mpc);
      return(ret);
   }

   mpc->mpc_mpa_delay[CLR_LOOP_BACK_INDEX] = 0;

   if (!(state & MPA_UART_CFG_DONE)) {
      return(0xFF);
   }

   if (!(state & MPA_SET_LOOP_BACK_DONE)) {
      return(0xFF);
   }

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);

   mpc->mpc_mpa_stat |= MPA_INITG_CLR_LOOP_BACK;
   curmarb->sspcb = icp->sspcb; 
   curmarb->slot_chan = slot_chan;
   curmarb->scratch_area = NULL;
   curmarb->req_type = 0xFF;    /* NON-WAITED */

   retv = mpa_srvc_clr_loop_back( curmarb );

   if (retv == 0) {
      MESSAGE("SUCCESS: clr_loop_back", retv, mpc);
      mpc->mpc_mpa_stat &= ~MPA_INITG_CLR_LOOP_BACK;
      mpc->mpc_mpa_stat &= ~MPA_SET_LOOP_BACK_DONE;
      mpc->mpc_mpa_stat &= ~MPA_CLR_LOOP_BACK_ERROR;
      mpc->mpc_mpa_clb_wait = 0;
      wake_up_interruptible(&mpc->mpc_mpa_clb);
   } else if(retv == 0xFF00)  {
      /* waited call schedule more work */
   } else    {
      ramp_clr_loop_back_error(mpc, retv);
      ret = retv;
   }
   return(ret);
}

/*
** ramp_start_break_error(mpc, retv)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_start_break_error( struct mpchan *mpc, int retv)
{
   mpc->mpc_mpa_stat &= ~MPA_INITG_START_BREAK;
   if (mpc->mpc_mpa_cout_ctrl_sigs) {
      SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
      mpc->mpc_mpa_cout_ctrl_sigs = 0;
   }
   MESSAGE("ERROR: start_break", retv, mpc);
}

/*
** ramp_stop_break_error(mpc, retv)
**
** Resets mpa_stat flag and displays error message.
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_stop_break_error( struct mpchan *mpc, int retv)
{
   mpc->mpc_mpa_stat &= ~MPA_INITG_STOP_BREAK;
   if (mpc->mpc_mpa_cout_ctrl_sigs) {
      SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
      mpc->mpc_mpa_cout_ctrl_sigs = 0;
   }
   MESSAGE("ERROR: stop_break", retv, mpc);
}

/*
** ramp_hard_reset_error(mpc, retv)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_hard_reset_error( struct mpchan *mpc, int retv)
{
   mpc->mpc_mpa_stat &= ~MPA_INITG_HARD_RESET;
   mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;  
   if (mpc->mpc_mpa_cout_ctrl_sigs) {
      SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
      mpc->mpc_mpa_cout_ctrl_sigs = 0;
   }
   MESSAGE("ERROR: hard_reset", retv, mpc);
      mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_CLR_LOOP_BACK_ERROR;
	mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
   mpc->mpc_mpa_slb_wait = 0;
   mpc->mpc_mpa_clb_wait = 0;
   mpc->mpc_mpa_rst_wait = 0;
   mpc->mpc_mpa_call_back_wait = 0;
   wake_up_interruptible(&mpc->mpc_mpa_slb);
   wake_up_interruptible(&mpc->mpc_mpa_clb);
   wake_up_interruptible(&mpc->mpc_mpa_rst);
   wake_up_interruptible(&mpc->mpc_mpa_call_back);
}

/*
** ramp_init_modem_error(mpc, retv)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_init_modem_error( struct mpchan *mpc, int retv)
{
   PRINT("ERROR: init_modem", retv, mpc);
   mpc->mpc_mpa_stat |= MPA_INIT_MODEM_ERROR;
   mpc->mpc_mpa_stat &= ~MPA_INITG_INIT_MODEM;
   mpc->mpc_mpa_stat &= ~MPA_INITG_MODIFY_SETTINGS;
   if (mpc->mpc_mpa_cout_ctrl_sigs) {
      SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
      mpc->mpc_mpa_cout_ctrl_sigs = 0;
   }
      mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_CLR_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;
	mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
   mpc->mpc_mpa_slb_wait = 0;
   mpc->mpc_mpa_clb_wait = 0;
   mpc->mpc_mpa_rst_wait = 0;
   mpc->mpc_mpa_call_back_wait = 0;
   wake_up_interruptible(&mpc->mpc_mpa_slb);
   wake_up_interruptible(&mpc->mpc_mpa_clb);
   wake_up_interruptible(&mpc->mpc_mpa_rst);
   wake_up_interruptible(&mpc->mpc_mpa_call_back);
}

/*
** ramp_dereg_modem_cleanup(mpc)
**
** Called to clean up after a deregister modem.
**
** This routine will perform all the necessary wakeups and signals to
** ensure that no process is left sleeping.
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_dereg_modem_cleanup( struct mpchan *mpc)
{

   register struct tty_struct  *tp;
   int i;


   /* Make sure everyone wakes up */
   if (mpc->mpc_mpa_cout_ctrl_sigs) {
      SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
      mpc->mpc_mpa_cout_ctrl_sigs = 0;
   }
   mpc->mpc_flags &= ~MPC_BUSY;
      mpc->mpc_mpa_stat &= ~MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_stat &= ~MPA_CLR_LOOP_BACK_ERROR;
   mpc->mpc_mpa_stat |= MPA_HARD_RESET_ERROR;
   mpc->mpc_mpa_stat |= MPA_CALL_BACK_ERROR;
   mpc->mpc_mpa_slb_wait = 0;
   mpc->mpc_mpa_clb_wait = 0;
   mpc->mpc_mpa_rst_wait = 0;
   mpc->mpc_mpa_call_back_wait = 0;
   wake_up_interruptible(&mpc->mpc_mpa_slb);
   wake_up_interruptible(&mpc->mpc_mpa_rst);
   wake_up_interruptible(&mpc->mpc_mpa_clb);
   wake_up_interruptible(&mpc->mpc_mpa_call_back);

   /* Clean up status flag */
   mpc->mpc_mpa_stat = 0;
   mpc->mpc_mpa_init_retry = 0;
   mpc->mpc_mpa_reset_retry = 0;

   /* Clean up pending commands */
   for(i = 0; i < MAX_RAMP_INDEX; i++)
      mpc->mpc_mpa_delay[i] = 0;

	mpc->carr_state = 0;
	tp = mpc->mpc_tty;
	if (tp == (struct tty_struct *) NULL)
		return;
   /* Send SIGHUP if open */
#if	(LINUX_VERSION_CODE < 132608)
   /* 2.2 and 2.4 kernels */
   if (!((mpc->flags & ASYNC_CALLOUT_ACTIVE) &&
	(mpc->flags & ASYNC_CALLOUT_NOHUP))){
#else
   /* 2.6 kernels */
   if(mpc->flags & ASYNC_CALLOUT_NOHUP) {
#endif

#ifdef DEBUG
		printk("ramp_dereg_cleanup: calling tty_hangup for device %d\n", SSTMINOR(mpc->mpc_major, mpc->mpc_minor));
#endif
		tty_hangup(tp);
	}
}

/*
 * ramp_fsm(mpc)
 *
 * run from eqn_ramp_admin.
 *
 * If break is off, modem in slot, so it will be registered.
 * If break is on, modem was removed, so it will be deregistered.
 *
 * If neither needs to be done, see if anything is doing call backs.
 * If so, go call back. If not, look at the array of functions which
 * are pending waiting on something else to be done. If one is pending,
 * run it.
 *
 * The pending function array is an array of characters. The value of the
 * array element is the order to run the function, with the lowest value
 * being the first to run. The index of the array represents the function
 * to run. See eqnx.h for the values of the array indices. This represents
 * only a one level memory of functions to run, but no single function
 * should be called again before it has run. If there is an attempt to
 * do so, the function will be called only once, the subsequent calls
 * will be silently "forgotten". 
 * 
 * mpdev board lock ** MUST ** be held			 
*/
void ramp_fsm( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int slot_chan, port, lmx;
   icpiaddr_t icpi;
   icpbaddr_t icpb;
   int win16, index;

#ifdef	DEBUG_LOCKS
   if (!(spin_is_locked(&mpc->mpc_mpd->mpd_lock))) {
	printk("LOCK Failure: mpd board lock NOT locked in ramp_fsm()\n");
   }
#endif	/* DEBUG_LOCKS */

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);
   win16 = mpc->mpc_mpd->mpd_nwin;
   if(win16)
      mega_push_win(mpc, 0);

   /* check break bit */
   /* If not receiving a break, the slot is occupied by a UART */
   icpi = mpc->mpc_icpi;
   icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
      
   if (mpc->mpc_mpa_stat == 0) {
      /* Initial state */
      if (!(icpb->ssp.bank_sigs & 0x01 )) {
#ifdef DEBUG_RAMP
	 printk("calling ramp_reg_modem for %d\n", mpc->mpc_chan);
#endif
         ramp_reg_modem(mpc);
      }
   } else if ((mpc->mpc_mpa_stat & MPA_UART_CFG_DONE)
      && (icpb->ssp.bank_sigs & 0x01 )
      /* Resetting some modems causes break bit to come on. Ignore it. */
      && (!(mpc->mpc_mpa_stat & MPA_INITG_HARD_RESET))) {
      /* modem configured and break detected
        (ie. modem unplugged) deregister it */
#ifdef DEBUG_RAMP
      printk("calling dereg_modem for %d \n", mpc->mpc_chan);
#endif
      ramp_dereg_modem(mpc);
   } else {
      /* If nothing pending, see if anything waiting to be done. */
      if (!(MPA_CALL_BACK(mpc->mpc_mpa_stat))) {
         if ((index = ramp_get_index(mpc)) && (index < MAX_RAMP_INDEX)) {
            mpc->mpc_mpa_delay[index] = 0;
            ramp_set_initg(mpc, index);
            ramp_set_index(mpc);
         }
      } else {
         /*printf("%s-%d: mpc_stat(%x) \n",__FILE__,__LINE__,mpc->mpc_mpa_stat)*/;
      }

      if (mpc->mpc_mpa_stat & MPA_INITG_UART) {
         ramp_reg_modem(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_HARD_RESET) {
         ramp_hard_reset(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_SET_LOOP_BACK) {
         ramp_set_loop_back(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_CLR_LOOP_BACK) {
         ramp_clr_loop_back(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_START_BREAK) {
         ramp_start_break(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_STOP_BREAK) {
         ramp_stop_break(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_INIT_MODEM) {
         ramp_init_modem(mpc);
      } else if (mpc->mpc_mpa_stat & MPA_INITG_MODIFY_SETTINGS) {
         ramp_init_modem(mpc);
      }
   }
   if(win16)
      mega_pop_win(50);
   if ((index = ramp_get_index(mpc)))
      ;
}

/******************************************************************************

        EQN_RAMP_ADMIN

        Scheduled RAMP administration routine.
        
        If any channels are referenced, eqn_push_win()/eqn_pop_win() 
	must be called.

	This is scheduled initially from mega_rng_delta upon successful
	completion of init_mpa. It is first run 2.1 seconds after the
	init mpa completes. This delay gives modems which emulate a UART
	time to initialize. After this, it is scheduled every 200 ms.
	when no call back is active. It is scheduled every 10 ms. when
	a call back is active.
******************************************************************************/
        
void eqn_ramp_admin(unsigned long arg)
{
struct mpdev *mpd;
struct mpchan *mpc;
struct icp_struct *icp;
struct lmx_struct *lmxp;
int slot_chan, port, nicp, lmx;
int i;
unsigned long flags;
#ifdef DEBUG
	icpgaddr_t icpg;
#endif

  to_eqn_ramp_admin = -1;

  ramp_admin_poll = MPRAMPTIME0;

  /* check each icp on each mpd */
   for (i=0; i < (int)nmegaport; i++) {	/* adapters loop */
      mpd = &meg_dev[i];
      if (mpd->mpd_alive != DEV_GOOD) 
         continue; /* If board is dead skip it */

    /* if not 64 or 128 port board skip it  */
    if (mpd->mpd_board_def->number_of_ports < 64)
    {
       continue;
    }

    /*
    ** lock mpdev board lock
    */
    spin_lock_irqsave(&mpd->mpd_lock, flags);

    for ( nicp = 0; nicp < mpd->mpd_nicps; nicp++ )
    {
#ifdef DEBUG
icpg = (icpgaddr_t)((unsigned long)mpd->icp[nicp].icp_regs_start + 0x2000);
	printk("ramp_admin_poll = %d, frame counter = %d\n", ramp_admin_poll,
Gicp_fram_ctr);
#endif
      icp = mpd->mpd_mpc->mpc_icp + nicp;
      for(lmx = 0; lmx < 4; lmx++)
      {
         lmxp = &(icp->lmx[lmx]);
         switch(lmxp->lmx_id)
         {
             case 8:
             case 9:
             case 0xB:	/* fall thru */
                  break;
             default:  /* not a RAMP module */
                  continue;
         } 

         for ( port = 0; port < lmxp->lmx_chan; port++ )
         {
             
            slot_chan = port + (lmx * 16);
            /* process this channel */
            mpc = mpd->mpd_mpc + (nicp * MAXICPCHNL) + (lmx * MAXLMXMPC) + port;

            if(lmxp->lmx_active != DEV_GOOD)
            {
                /* ldv went away: clear status word */
                mpc->mpc_mpa_stat = 0;
                continue;
            }

            if(mpc->mpc_mpa_stat & MPA_INIT_ERROR) {
               if (mpc->mpc_mpa_init_retry < MPA_MAX_RETRY) {
                  unsigned int win16;
                  mpc->mpc_mpa_init_retry++;

                  win16 = mpc->mpc_mpd->mpd_nwin;
                  if(win16)
                     mega_push_win(mpc, 0);
                  ramp_dereg_modem(mpc);
                  if(win16)
                     mega_pop_win(59);
                  mpc->mpc_mpa_stat = 0;
               } else if (mpc->mpc_mpa_reset_retry < MPA_MAX_RETRY) {
                  unsigned int win16;
                  mpc->mpc_mpa_reset_retry++;

                  win16 = mpc->mpc_mpd->mpd_nwin;
                  if(win16)
                     mega_push_win(mpc, 0);
                  ramp_hard_reset(mpc);
                  if(win16)
                     mega_pop_win(60);
               } else {
                  continue;
               }
            } else if ((mpc->mpc_mpa_stat & MPA_LOOP_ERROR) ||
                       (mpc->mpc_mpa_stat & MPA_BREAK_ERROR) ||
                       (mpc->mpc_mpa_stat & MPA_INIT_MODEM_ERROR) ||
                       (mpc->mpc_mpa_stat & MPA_HARD_RESET_ERROR)) {
                  continue;
            }
            /* Handle MPA states */
            ramp_fsm(mpc);

         }   /* for all RAMP ports */
      }  /* for each lmx */
    } /* for each ICP */
   
    spin_unlock_irqrestore(&mpd->mpd_lock, flags);

  }  /* adapter's loop */

/*-------------------------------------------------------*/

   mpa_srvc_diag_poll();

  /* reschedule */
	eqnx_ramp_timer.expires = jiffies + ramp_admin_poll;
	add_timer(&eqnx_ramp_timer);
	to_eqn_ramp_admin = 1;
}  /* eqn_ramp_admin */

/*
 * RAMP_DEREG_MODEM
 *
 * deregisters modem unconditionally. Following completion of the
 * deregister, the cleanup routine will ensure that any sleepers
 * are revived.
 * 
 * mpdev board lock ** MUST ** be held			 
 */
void ramp_dereg_modem( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int chars;
   int slot_chan, port, lmx, retv;
   icpiaddr_t icpi;
   icpbaddr_t icpb;

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);
   curmarb->sspcb = icp->sspcb; 
   curmarb->req_type = 0x00;    /* WAITED: unconditionly */
   curmarb->slot_chan = slot_chan;
   curmarb->scratch_area = NULL;
   /* flush the input buffer first: this is necessary */
   icpi = mpc->mpc_icpi;
   icpb = (rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
   chars = icpb->ssp.bank_num_chars;
   if(chars) {
      eqnrflush(mpc);
      return;
   }
   retv = mpa_srvc_dereg_modem( curmarb );


   if(retv == 0) {
      MESSAGE("SUCCESS: dereg_modem", retv, mpc);
      if (!POWER_STATE(mpc)) {
         mpc->mpc_mpa_stat = 0;
         mpc->mpc_mpa_init_retry = 0;
         mpc->mpc_mpa_reset_retry = 0;
      }
   } else {
      MESSAGE("ERROR: dereg_modem", retv, mpc);
      ramp_check_error(mpc, retv);
   }
   ramp_dereg_modem_cleanup(mpc);
}

/*
** eqnrflush(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
static void eqnrflush( struct mpchan *mpc)
{
	icpbaddr_t icpb;
	icpiaddr_t icpi = mpc->mpc_icpi;

	if ((mpc->mpc_icpi->ssp.cin_locks & 0x01) == 1)
		icpb = &icpi->ssp.cin_bank_b;
	else
		icpb = &icpi->ssp.cin_bank_a;
	mpc->mpc_rxq.q_ptr = rx_next + (rx_fifo_cnt & 0xf);
	if(mpc->mpc_mpd->mpd_nwin)
		mpc->mpc_rxq.q_ptr &= 0x3fff;
	if (mpc->mpc_rxq.q_ptr > mpc->mpc_rxq.q_end)
		mpc->mpc_rxq.q_ptr = mpc->mpc_rxq.q_begin;
	if(mpc->mpc_mpd->mpd_nwin) {
		rx_tail(mpc->mpc_rxbase + mpc->mpc_rxq.q_ptr);
	}
	else {
		rx_tail(mpc->mpc_rxq.q_ptr);
	}
	mpc->mpc_flags |= MPC_RXFLUSH;
}   /* eqnrflush */

/*
 * These next few routines implement a one level queue of
 * commands to execute. Each command to execute is "remembered"
 * by placing a value in an array. The array index is the command
 * to "remember". The value is the order to execute the commands. 
 * The lowest value is executed first. This is how we keep from
 * calling non-waited calls when a previous non-waited call has
 * not yet finished. 
 */

/* When ramp_fsm finds no more commands to 
 * process, it will call the ramp_set_initg() routine to set the
 * state flag to the next command to process. 
 *
 * mpdev board lock ** MUST ** be held			 
 */
void ramp_set_initg( struct mpchan *mpc, int index)
{
#ifdef DEBUG_RAMP
   printk("ramp_set_initg:chan %d with index= %d\n", mpc->mpc_chan,index);
#endif
   switch(index) {
      case REG_MODEM_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_UART;
         break;
      case SET_LOOP_BACK_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_SET_LOOP_BACK;
         break;
      case CLR_LOOP_BACK_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_CLR_LOOP_BACK;
         break;
      case START_BREAK_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_START_BREAK;
         break;
      case STOP_BREAK_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_STOP_BREAK;
         break;
      case INIT_MODEM_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_INIT_MODEM;
         break;
      case MODIFY_SETTINGS_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_MODIFY_SETTINGS;
         break;
      case HARD_RESET_INDEX:
         mpc->mpc_mpa_stat |= MPA_INITG_HARD_RESET;
         break;
      default:
         mpc->mpc_mpa_stat |= MPA_INIT_ERROR;
         break;
   }
}

/*
 * ramp_get_index() is called to find the index of the command with
 * the lowest value. This is the command to execute next.
 *
 * mpdev board lock ** MUST ** be held			 
 */
int ramp_get_index( struct mpchan *mpc)
{
   int i;
   int min_index = 0;

   for (i = 0; i < MAX_RAMP_INDEX; i++) {
      if (mpc->mpc_mpa_delay[i]) {
         if (mpc->mpc_mpa_delay[min_index] == 0)
            min_index = i;
         else
            min_index=(mpc->mpc_mpa_delay[i] < mpc->mpc_mpa_delay[min_index]) ? 
               i : min_index;
      }
   }
   return(min_index);
}

/*
 * ramp_set_index() reduces the values of the members of the
 * array of commands to be remembered, so we don't have to worry 
 * about wrap around. These values should always be in the 
 * range (1 to MAX_RAMP_INDEX).
 *
 * mpdev board lock ** MUST ** be held			 
 */
void ramp_set_index( struct mpchan *mpc)
{
   int i;

   for(i = 0; i < MAX_RAMP_INDEX; i++) {
      if (mpc->mpc_mpa_delay[i]) {
         mpc->mpc_mpa_delay[i]--;
      }
   }
}

/*
 * ramp_get_delay () finds the largest current value in the commands
 * array. One more than this is returned as the value to put in the
 * array for the next command to remember.
 *
 * mpdev board lock ** MUST ** be held			 
 */
int ramp_get_delay( struct mpchan *mpc)
{
   int i;
   int max_index = 0;

   for (i = 0; i < MAX_RAMP_INDEX; i++) {
      max_index = (mpc->mpc_mpa_delay[i] > max_index) ? mpc->mpc_mpa_delay[i] : max_index;
   }
   return(max_index + 1);
}

/*
** ramp_init_modem(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_init_modem( struct mpchan *mpc)
{

   struct mpdev *mpd;
   int rslt = 0;  /* default */
   struct icp_struct *icp;
   unsigned speed;
   ushort_t d;
   struct marb_struct oldmarb;
   struct marb_struct *curmarb;
   volatile register struct termios *tiosp;
   icpiaddr_t icpi;
   icpoaddr_t icpo;
   int slot_chan, ii, lmx, port, retv;
   uchar_t cur;
   int updated = 0;

   if (!(mpc->mpc_mpa_stat & MPA_UART_CFG_DONE)) {
      return;
   }

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);
   memset(&oldmarb, 0, sizeof( struct marb_struct));

   /* First time (not on call backs) set up signals, etc. */
   if (!(mpc->mpc_mpa_stat & (MPA_INITG_INIT_MODEM | 
	MPA_INITG_MODIFY_SETTINGS))) {
ramp_termios:
        updated = 1;
     if (mpc->mpc_tty != (struct tty_struct *) NULL){ /* shashi: 03/22/98 */
      /* setup pointers for general use */
        tiosp = mpc->mpc_tty->termios;
  	icpi = mpc->mpc_icpi;
  	icpo = mpc->mpc_icpo;
  	/* CLOCAL and carrier detect parameters */
  	if (tiosp->c_cflag & CLOCAL){
    		mpc->mpc_icpi->ssp.cin_attn_ena &= ~ENA_DCD_CNG;
		mpc->carr_state = 1;
  	}
  	else{  /* CLOCAL clear */
    		/* set DCD processing */
    		mpc->mpc_icpi->ssp.cin_attn_ena |= ENA_DCD_CNG;
	}

  	/* outbound control signals */
  	if ((tiosp->c_cflag & CBAUD) == 0)  /* B0 */
  	{
		/*if (!(tiosp->c_cflag & CLOCAL)){*/
    			(void) megamodem(SSTMINOR(mpc->mpc_major, mpc->mpc_minor), 
					TURNOFF);
		/*}*/
    		return;
  	}
	else{
		mpc->carr_state = megamodem(
			SSTMINOR(mpc->mpc_major, mpc->mpc_minor), 
			TURNON); /*shashi 02/04/98*/
	}

	/* set output control signal flow control */
	if(mpc->mpc_param & IOCTCTS) 
		tiosp->c_cflag |= CRTSCTS;

	/* set input control signal flow control */
	if(mpc->mpc_param & IOCTRTS) 
		tiosp->c_cflag |= CRTSCTS;
#if 0 /* Hardware flow done below for modem pool */
	if(tiosp->c_cflag & CRTSCTS)  /* only need to change the mask */
		icpi->ssp.cin_susp_output_lmx |= CTS_OFF;
	else
		icpi->ssp.cin_susp_output_lmx &= ~CTS_OFF;


	if (tiosp->c_cflag & CRTSCTS)
		tx_ctrl_sig &= ~TX_HFC_RTS;
	else
        	tx_ctrl_sig |= TX_HFC_RTS;
  
#endif

      /* set output inband flow control */
      if(tiosp->c_iflag & IXON || mpc->mpc_param & IXONSET) {
         icpi->ssp.cin_xoff_1 = mpc->mpc_stop;
         icpi->ssp.cin_xon_1 = mpc->mpc_start;
         icpi->ssp.cin_char_ctrl |= EN_XON|EN_XOFF;
         icpi->ssp.cin_locks &= ~DIS_IBAND_FLW;  /* clear lock bit - enable inband */
         cur_chnl_sync( mpc );
         icpi->ssp.cin_inband_flow_ctrl = 0; /* calibrate */
         if(mpc->mpc_param & IOCTXON)
            icpi->ssp.cin_char_ctrl &= ~EN_DNS_FLW;  /* do not discard xon/xoff */
         else
            icpi->ssp.cin_char_ctrl |= EN_DNS_FLW;  /* discard xon/xoff */
         icpi->ssp.cin_char_ctrl &= ~EN_DBL_FLW; /* disable double flow */
         if (( tiosp->c_iflag & IXANY ) && !(mpc->mpc_param & IXANYIG)) 
            icpi->ssp.cin_char_ctrl |= EN_IXANY;
         else
            icpi->ssp.cin_char_ctrl &= ~EN_IXANY;
      } else  /* cancel action */ {
         icpi->ssp.cin_char_ctrl &= ~EN_DNS_FLW;  /* discard xon/xoff */
         icpi->ssp.cin_locks |= DIS_IBAND_FLW;     /* set lock bit - disable inband */
         cur_chnl_sync( mpc );
         icpi->ssp.cin_inband_flow_ctrl = 0;  /* calibrate */
      }
      icpo->ssp.cout_xoff_1 = mpc->mpc_stop;
      icpo->ssp.cout_xon_1 = mpc->mpc_start;
      /* set input inband flow control */
      if ( tiosp->c_iflag & IXOFF ) {
         icpo->ssp.cout_flow_cfg &= ~TX_XON_DBL;
         icpo->ssp.cout_flow_cfg |= TX_XON_XOFF_EN;
      } else  /* cancel enable, send XON if we previously sent an XOFF */ {
         if ((icpo->ssp.cout_flow_cfg & TX_XON_XOFF_EN) && (icpi->ssp.cin_int_flags & 0x40)) {
            megajam(mpc, mpc->mpc_start);
         }
         icpo->ssp.cout_flow_cfg &= ~(TX_XON_XOFF_EN|TX_XON_DBL);
      }

      /* DEBUG: this fixes data errors */
      cur = mpc->mpc_cout->ssp.cout_lmx_cmd & 0x03; /* preserve RAMP bits */
      cur |= 0x68;
      cur &= ~0x40;
      mpc->mpc_cout->ssp.cout_lmx_cmd = cur;
   
      /* get a copy of the old marb values */
      memcpy(&oldmarb, curmarb, sizeof( struct marb_struct));

      curmarb->sspcb = mpc->mpc_icp->sspcb; 
      curmarb->slot_chan = slot_chan;
      curmarb->scratch_area = NULL;
      curmarb->req_type = 0xFF;    /* NON-WAITED */
      curmarb->sup_uchar0 = 0x00;

/* Tell the MPA to handle HW flow control */
      if(tiosp->c_cflag & CRTSCTS)  /*  Bit 3     IN      CTS	*/
      	curmarb->sup_uchar0 |= 0x08;
      if (tiosp->c_cflag & CRTSCTS)  /*  Bit 1     OUT     RTS	*/
      	curmarb->sup_uchar0 |= 0x02;

      /* baudrate */
      speed = tiosp->c_cflag & CBAUD;
      if (speed & CBAUDEX){
	speed &= ~CBAUDEX;
	if ((speed < 1) || (speed > 4))
		tiosp->c_cflag &= ~CBAUDEX;
	else 
		speed += 15;
      }
      if ( speed > 0 && speed <= (B38400 + 4) ) {

         /* RAMP */
         curmarb->sup_ushort = uart_baud(speed, mpc); 
   
      } else
         rslt = 1;  /* no valid speed to set */

      /* databits */
      switch (tiosp->c_cflag & CSIZE){  /* set character size */
         case CS5:
            d = 0x00;
            break;

         case CS6:
            d = 0x01;
            break;

         case CS7:
            d = 0x02;
            break;

         default:  /* case CS8 - no other value should occur */
            d = 0x03;
      }
      /* "d" has the correct value for RAMP also */
      curmarb->sup_uchar1 = d;
   
      if(tiosp->c_cflag & PARENB){
            d |= 0x04;
    	    if( !(tiosp->c_cflag & PARODD) ) /* even parity */ {
               d |= 0x08;
               curmarb->sup_uchar3 = 0x18;    /* RAMP: even parity generation */
            } else		
               curmarb->sup_uchar3 = 0x08;    /* RAMP: odd parity generation */
      } else
         curmarb->sup_uchar3 = 0x00;    /* RAMP: no parity generation */

      /* DEBUG LAST CHANGE */
      /* prepare for input break/parity processing - cancel special features */
      mpc->mpc_cin_stat_mask &= ~0x03a0;

      /* prepare for input break processing */
      if ( tiosp->c_iflag & IGNBRK ){
         rslt = 0; /* get rid of empty "if" */
      } else {
         /* hardware must maintain break nulls */
         mpc->mpc_cin_stat_mask |= 0x0120;   /* watch for framing errs w/nulls */
      }

      /* clear lookup table - processing flags already clear */
      for ( ii = 0; ii < 32; ii++ )
         mpc->mpc_cin->ssp.cin_lookup_tbl[ii] = 0;

      /* prepare for input parity processing */
  if ( tiosp->c_cflag & PARENB && tiosp->c_iflag & INPCK ) {
    	if ( tiosp->c_iflag & IGNPAR ) {
            rslt = 0; /* get rid of empty "if" */
    	}
    	else{
            /* hardware must maintain and tag err'd chars */
            mpc->mpc_cin_stat_mask |= 0x0180; /* watch for parity and framing */
      	    if ( tiosp->c_iflag & PARMRK && !(tiosp->c_iflag & ISTRIP) ){
               /* put 0xff in lookup table */
               mpc->mpc_cin->ssp.cin_lookup_tbl[0x1f] |= 0x80;
               mpc->mpc_cin_stat_mask |= 0x0200;     /* watch for lookup event */
            }
         }
      }

      /* output stop bits */
      if (tiosp->c_cflag & CSTOPB){
         /* RAMP: 2 stop bits */
         curmarb->sup_uchar2 = 0x04;
      } else {
         /* RAMP: 1 stop bits */
         curmarb->sup_uchar2 = 0x00;
      }
     } /* port tty valid */
     else{
	mpc->mpc_mpa_call_back_wait = 0;
      	wake_up_interruptible(&mpc->mpc_mpa_call_back);
     	return;
     }
   } /* First time */

   /* RAMP */
   /* If no values have actualy been modified don't call init_modem. */
   /* ksh needlessly issues ioctls when the user hits the enter key. */
   /* This almost guarantees that data will be in transit when we    */
   /* call this service. Which breaks the init_modem call            */
         
   if( (mpc->mpc_mpa_stat & MPA_INIT_MODEM_DONE)
      && curmarb->sup_uchar0 == oldmarb.sup_uchar0
      && curmarb->sup_uchar1 == oldmarb.sup_uchar1
      && curmarb->sup_uchar2 == oldmarb.sup_uchar2
      && curmarb->sup_uchar3 == oldmarb.sup_uchar3
      && curmarb->sup_ushort == oldmarb.sup_ushort ) {
         retv = 0;
	mpc->mpc_mpa_call_back_wait = 0;
      	wake_up_interruptible(&mpc->mpc_mpa_call_back);
	 return;
   } else if ((mpc->mpc_mpa_stat & MPA_INITG_MODIFY_SETTINGS) ||
             (mpc->mpc_mpa_stat & MPA_INIT_MODEM_DONE)) {
      mpc->mpc_mpa_stat &= ~MPA_INIT_MODEM_DONE;
      mpc->mpc_mpa_stat |= MPA_INITG_MODIFY_SETTINGS;
      retv = mpa_srvc_modify_settings( curmarb );
   } else {
      mpc->mpc_mpa_stat |= MPA_INITG_INIT_MODEM;
      retv = mpa_srvc_init_modem( curmarb );
   }
   if(retv == 0xFF00) {
      ;
   } else if (retv) {
      ramp_check_error(mpc, retv);
   } else {
      MESSAGE("SUCCESS: init_modem", retv, mpc);
      ramp_modem_cleanup(mpc);
      mpc->mpc_mpa_stat |= MPA_INIT_MODEM_DONE;
      mpc->mpc_mpa_stat &= ~MPA_INITG_INIT_MODEM;
      mpc->mpc_mpa_stat &= ~MPA_INITG_MODIFY_SETTINGS;
      if (mpc->mpc_mpa_cout_ctrl_sigs) {
         SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
         mpc->mpc_mpa_cout_ctrl_sigs = 0;
      }
	mpc->mpc_mpa_stat &= ~MPA_CALL_BACK_ERROR;
      if (!updated)
	goto ramp_termios;
      else {
	mpc->mpc_mpa_call_back_wait = 0;
      	wake_up_interruptible(&mpc->mpc_mpa_call_back);
      }
   }

} /* ramp_init_modem */

/*
** ramp_modem_cleanup(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_modem_cleanup( struct mpchan *mpc)
{
   icpiaddr_t icpi;
   icpoaddr_t icpo;

   icpi = mpc->mpc_icpi;
   icpo = mpc->mpc_icpo;

   /* set cin_tmr_preset_count and enable */
   mpc->mpc_cin->ssp.cin_locks |= 0x10;
   mpc->mpc_cin->ssp.cin_tmr_preset_count = 1;
   mpc->mpc_cin->ssp.cin_char_tmr_remain = 0;
   mpc->mpc_cin->ssp.cin_tmr_size = 0;
   mpc->mpc_cin->ssp.cin_locks &= ~0x10;

}

/*
** ramp_hard_reset(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_hard_reset( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int chars;
   int slot_chan, port, lmx, retv;
   icpiaddr_t icpi;
   icpbaddr_t icpb;

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);
#ifdef DEBUG_RAMP
   printk("hard_reset: for channel %d\n", mpc->mpc_chan);
#endif

   /* If doing non-waited call, try again later. */
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~MPA_INITG_HARD_RESET)) {
      if (mpc->mpc_mpa_delay[HARD_RESET_INDEX] == 0) {
         mpc->mpc_mpa_delay[HARD_RESET_INDEX] = ramp_get_delay(mpc); 
      }
      return;
   }

   icpi = mpc->mpc_icpi;
   icpb=(rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
   if (!(mpc->mpc_mpa_stat & MPA_INITG_HARD_RESET)) {

      /* flush the input buffer first: this is necessary */
      chars = icpb->ssp.bank_num_chars;

      if(chars)  {
         mpc->mpc_mpa_stat |= MPA_INITG_HARD_RESET;
         eqnrflush(mpc);
         return;
      }
   }

   mpc->mpc_mpa_stat |= MPA_INITG_HARD_RESET;

   if (mpc->mpc_flags & MPC_BUSY)
      return;

   curmarb->sspcb = icp->sspcb; 
   curmarb->slot_chan = slot_chan;
   curmarb->scratch_area = NULL;
   curmarb->req_type = 0xFF;    /* NON-WAITED */

   retv = mpa_srvc_hard_reset( curmarb );

   if (retv == 0) {
      MESSAGE("SUCCESS: hard_reset", retv, mpc);
      /* Clear false LMX change */
      FREEZ_BANK(mpc);
      FREEZ_BANK(mpc);
      mpc->mpc_cin_events = 0;
      mpc->mpc_mpa_stat &= ~MPA_INITG_HARD_RESET;
      mpc->mpc_mpa_init_retry = 0;
      mpc->mpc_mpa_reset_retry = 0;
      if (mpc->mpc_mpa_cout_ctrl_sigs) {
         SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
         mpc->mpc_mpa_cout_ctrl_sigs = 0;
      }
      mpc->mpc_mpa_rst_wait = 0;
      wake_up_interruptible(&mpc->mpc_mpa_rst);
   } else if(retv == 0xFF00)  {
      /* waited call schedule more work */
   } else    {
      ramp_hard_reset_error(mpc, retv);
   }
}

/*
** ramp_set_loop_back(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
int ramp_set_loop_back( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int slot_chan, port, lmx, retv, ret = 0;
   unsigned int state;

   /* If doing non-waited call, try again later. */
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~MPA_INITG_SET_LOOP_BACK)) {
      if (mpc->mpc_mpa_delay[SET_LOOP_BACK_INDEX] == 0) {
         mpc->mpc_mpa_delay[SET_LOOP_BACK_INDEX] = ramp_get_delay(mpc); 
      }
      return(ret);
   }

   mpc->mpc_mpa_delay[SET_LOOP_BACK_INDEX] = 0;

   state = mpc->mpc_mpa_stat;

   if (!(state & MPA_UART_CFG_DONE)) {
      return(0xFF);
   }


   if (state & MPA_SET_LOOP_BACK_DONE) {
      return(0xFF);
   }

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);

   curmarb->sspcb = icp->sspcb; 
   curmarb->slot_chan = slot_chan;
   curmarb->scratch_area = NULL;
   curmarb->req_type = 0xFF;    /* NON-WAITED */
   mpc->mpc_mpa_stat |= MPA_INITG_SET_LOOP_BACK;

   retv = mpa_srvc_set_loop_back( curmarb );

   if (retv == 0) {
      MESSAGE("SUCCESS: set_loop_back", retv, mpc);
      mpc->mpc_mpa_stat &= ~MPA_INITG_SET_LOOP_BACK;
      mpc->mpc_mpa_stat |= MPA_SET_LOOP_BACK_DONE;
      mpc->mpc_mpa_stat &= ~MPA_SET_LOOP_BACK_ERROR;
      mpc->mpc_mpa_slb_wait = 0;
      wake_up_interruptible(&mpc->mpc_mpa_slb);
   } else if(retv == 0xFF00)  {
      /* waited call schedule more work */
      ;
   } else    {
      ramp_set_loop_back_error(mpc, retv);
      ret = retv;
   }
   return(ret);
}

/*
** ramp_start_break(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
int ramp_start_break( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int slot_chan, port, lmx, retv, ret = 0;
   unsigned int state = mpc->mpc_mpa_stat;

   /* If doing non-waited call, try again later. */
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~MPA_INITG_START_BREAK)) {
      if (mpc->mpc_mpa_delay[START_BREAK_INDEX] == 0)
         mpc->mpc_mpa_delay[START_BREAK_INDEX] = ramp_get_delay(mpc); 
      return(ret);
   }

   mpc->mpc_mpa_delay[START_BREAK_INDEX] = 0;

   if (!(state & MPA_UART_CFG_DONE))
      return(0xFF);

   if ((state & MPA_START_BREAK_DONE))
      return(ret);

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);

   mpc->mpc_mpa_stat |= MPA_INITG_START_BREAK;
   curmarb->sspcb = icp->sspcb; 
   curmarb->slot_chan = slot_chan;
   curmarb->req_type = 0xFF; /* NON-WAITED */

   retv = mpa_srvc_start_break( curmarb );

   if (retv == 0) {
      MESSAGE("SUCCESS: start_break", retv, mpc);
      mpc->mpc_mpa_stat &= ~MPA_INITG_START_BREAK;
      mpc->mpc_mpa_stat |= MPA_START_BREAK_DONE;
      if (mpc->mpc_mpa_cout_ctrl_sigs) {
         SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
         mpc->mpc_mpa_cout_ctrl_sigs = 0;
      }
   } else if(retv == 0xFF00)  {
      /* waited call schedule more work */
      ;
   } else    {
      ramp_start_break_error(mpc, retv);
      ret = retv;
   }
   return(ret);
}

/*
** ramp_stop_break(mpc)
**
** mpdev board lock ** MUST ** be held			 
*/
void ramp_stop_break( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int slot_chan, port, lmx, retv;
   unsigned int state = mpc->mpc_mpa_stat;

   if (!(state & MPA_UART_CFG_DONE))
      return;

   /* If doing non-waited call, try again later. */
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat & ~MPA_INITG_STOP_BREAK)) {
      if (mpc->mpc_mpa_delay[STOP_BREAK_INDEX] == 0)
         mpc->mpc_mpa_delay[STOP_BREAK_INDEX] = ramp_get_delay(mpc); 
      return;
   }

   mpc->mpc_mpa_delay[STOP_BREAK_INDEX] = 0;

   if (!(state & MPA_START_BREAK_DONE))
      return;

   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);

   mpc->mpc_mpa_stat |= MPA_INITG_STOP_BREAK;
   curmarb->sspcb = icp->sspcb; 
   curmarb->slot_chan = slot_chan;
   curmarb->req_type = 0xFF; /* NON-WAITED */

   retv = mpa_srvc_stop_break( curmarb );

   if (retv == 0) {
      MESSAGE("SUCCESS: stop_break", retv, mpc);
      mpc->mpc_mpa_stat &= ~MPA_INITG_STOP_BREAK;
      mpc->mpc_mpa_stat &= ~MPA_START_BREAK_DONE;
      if (mpc->mpc_mpa_cout_ctrl_sigs) {
         SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
         mpc->mpc_mpa_cout_ctrl_sigs = 0;
      }
   } else if(retv == 0xFF00) {
      /* waited call schedule more work */
      ;
   } else {
      ramp_stop_break_error(mpc, retv);
   }
   return;
}

/*
 * RAMP_REG_MODEM
 *
 * Registers modem and wakes up sleepers when done.
 *
 * mpdev board lock ** MUST ** be held			 
 */
void ramp_reg_modem( struct mpchan *mpc)
{
   struct mpdev *mpd;
   struct marb_struct *curmarb;
   struct icp_struct *icp;
   int chars;
   int slot_chan, port, lmx, retv = 0;
   icpiaddr_t icpi;
   icpbaddr_t icpb;
#ifdef DEBUG_RAMP
   printk("register modem for channel %d\n", mpc->mpc_chan);
#endif
   PARAM_SETUP(mpc, mpd, slot_chan, port, lmx, icp, curmarb);

   if (!(mpc->mpc_mpa_stat & MPA_INITG_UART)) {

      /* flush the input buffer first: this is necessary */
      icpi = mpc->mpc_icpi;
      icpb=(rx_locks & LOCK_A) ? &icpi->ssp.cin_bank_b : &icpi->ssp.cin_bank_a;
      chars = icpb->ssp.bank_num_chars;

      if(chars)  {
         eqnrflush(mpc);
#ifdef DEBUG_RAMP
   printk("returning from register modem for channel %d because of chars\n", mpc->mpc_chan);
#endif
         return;
      }
   }

#ifdef DEBUG_RAMP
   printk("setting MPA_INITG_UART for channel %d\n", mpc->mpc_chan);
#endif
   mpc->mpc_mpa_stat = MPA_INITG_UART;
   curmarb->sspcb = icp->sspcb; 
   curmarb->slot_chan = slot_chan;
   curmarb->scratch_area = NULL;
   curmarb->req_type = 0xFF;    /* NON-WAITED */
   curmarb->sup_uchar0 = 0x00; /* no MPA to UART/Modem flow control */
   curmarb->sup_uchar1 = 3; /* 8 bits */
   curmarb->sup_uchar2 = curmarb->sup_uchar3 = 0; /* 1 stop, No parity */
   curmarb->sup_ushort = (115200/9600);

   retv = mpa_srvc_reg_modem( curmarb );
  
   if(retv == 0) {
      icpiaddr_t icpi;
      icpi = mpc->mpc_icpi;
#ifdef DEBUG_RAMP
      printk("SUCCESS reg_modem of type %x, ret = %d for channel %d\n", MODEM_TYPE(mpc), retv, mpc->mpc_chan);
#endif
      MESSAGE("SUCCESS reg_modem", retv, mpc);
      mpc->mpc_mpa_init_retry = 0;
      mpc->mpc_mpa_reset_retry = 0;
      mpc->mpc_cout->ssp.cout_cpu_req &= ~0x80;
      mpc->mpc_mpa_stat &= ~MPA_INITG_UART; 
      mpc->mpc_mpa_stat = MPA_UART_CFG_DONE; 
      if (mpc->mpc_mpa_cout_ctrl_sigs) {
         SET_CTRL_SIGS(mpc, mpc->mpc_mpa_cout_ctrl_sigs);
         mpc->mpc_mpa_cout_ctrl_sigs = 0;
      }
      /* Clear out flase LMX change */
      FREEZ_BANK(mpc);
      mpc->mpc_cin_events &= ~EV_LMX_CNG;

	mpc->mpc_mpa_stat &= ~MPA_CALL_BACK_ERROR;
      mpc->mpc_mpa_call_back_wait = 0;
      wake_up_interruptible(&mpc->mpc_mpa_call_back);
   } else if(retv ==0xFF00)  {
      /* waited call schedule more work */
#ifdef DEBUG_RAMP
   printk("waited call in reg_modem for channel %d\n", mpc->mpc_chan);
#endif
      ;

   } else {
         /* error */
      ramp_check_error(mpc, retv);
   }
}

/******************************************************************************
        
       RAMPARAM

	This routine configures a ramp channel as eqnparam does for non
	ramp devices. This must be performed using the ramp services.
	
	mpdev board lock ** MUST ** be held			 
	 
******************************************************************************/
static void ramparam( struct mpchan *mpc)
{

   /* If doing non-waited call, try again later. */
   if (MPA_CALL_BACK(mpc->mpc_mpa_stat)) {
      if ((mpc->mpc_mpa_stat & MPA_INIT_MODEM_DONE) ||
         (mpc->mpc_mpa_stat & MPA_INITG_MODIFY_SETTINGS)) {
         if (mpc->mpc_mpa_delay[MODIFY_SETTINGS_INDEX] == 0)
            mpc->mpc_mpa_delay[MODIFY_SETTINGS_INDEX] = ramp_get_delay(mpc); 
      } else {
         if (mpc->mpc_mpa_delay[INIT_MODEM_INDEX] == 0)
            mpc->mpc_mpa_delay[INIT_MODEM_INDEX] = ramp_get_delay(mpc);
      }
      return;
   }

   ramp_init_modem(mpc);
}

/******************************************************************************
   UART_BAUD Computes the uart divisor value for the RAMP services.

******************************************************************************/

static ushort_t uart_baud( int speed, struct mpchan *mpc)
{
int baud;
int val = icpbaud_tbl[speed];

  if (val == 38400){
  	if((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
		val = 57600;
  	else if((mpc->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
		val = 115200;
  }
  switch(val)
  {
     case 0:		baud = 0;
     case 50:		baud = 115200/50 ; break;
     case 75:		baud = 115200/75 ; break;
     case 110:		baud = 115200/110 ; break;
     case 134:		baud = 856 ; break;
     case 150:		baud = 115200/150 ; break;
     case 200:		baud = 115200/200 ; break;
     case 300:		baud = 115200/300 ; break;
     case 600:		baud = 115200/600 ; break;
     case 1200:		baud = 115200/1200 ; break;
     case 1800:		baud = 115200/1800 ; break;
     case 2400:		baud = 115200/2400 ; break;
     case 4800:		baud = 115200/4800 ; break;
     case 9600:		baud = 115200/9600 ; break;
     case 19200:	baud = 115200/19200 ; break;
     case 38400:	baud = 115200/38400 ; break;
     case 57600:	baud = 115200/57600 ; break;
     case 115200:	baud = 115200/115200 ; break;
     case 230400:	baud = 115200/115200 ; break;
     case 460800:	baud = 115200/115200 ; break;
     default:		baud = 0; break; /* can't be here */
 	
  }

  return(baud);
}  /* uart_baud */


/* End of code to save non-displayed messages */

#ifdef	IA64
/*
** simple byte copy.
** used because of problems with memcpy on IA64
*/
void memcpy_IA64(void * dest, void * src, int len)
{
	unsigned char	*destp, *srcp;
	int	i;

	destp = (unsigned char *) dest;
	srcp = (unsigned char *) src;
	for (i=0; i<len; i++)
		*(destp+i) = *(srcp+i);
}
#endif	/* IA64 */

#ifdef	ISA_ENAB
/*
** ISA find-a-hole function
*/

/************************************************************************
 * Structure definitions & defines/literals				*
 ************************************************************************/

#include <linux/version.h>
/*Literals used when searching for ROM											*/
#define ROM_BNDRY_SZ	0x800					/*Size (in bytes) to next possible ROM boundary		*/
#define ROM_AREA_BASE	0xC0000					/*Base address of area that can be occupied by ROM	*/
#define ROM_AREA_LMT	0xF4000					/*Address of top of ROM Area + 2k			*/
#define MAX_ROM_LOCS	((unsigned int)((ROM_AREA_LMT-ROM_AREA_BASE)/ROM_BNDRY_SZ)) /*# of possible ROM starting locs	*/

#define	ISA_ADDR_LOW	((int) 0x00)				/* Write (16-bits), addr bits 21 -14 */
#define ISA_PAGE_SEL	((int) 0x02)				/* write (16-bits), page select if page mode */

/*ICP displacements													*/
#define ICP_GLOBAL_DISPLC   ((unsigned int)0x2000)		/*Displacement from base of ICP regs to Global regs	*/

#define EQNX_VERSION_CODE LINUX_VERSION_CODE

#if (EQNX_VERSION_CODE < 131328)  /* kernel 2.0.x and below */
#define ioremap vremap
#define iounmap vfree
#endif

/************************
 *  Public Data Areas	*
 ************************/

	unsigned char			ux_ifns_rev = 0x05 ;		/*File's revision level				*/

	unsigned char			dynamic_cnfg_init = 0 ;		/*Dynamic configured ISA Controller flag:	*/
									/*	o 0x00 = Have not dynamically configured*/
									/*		 a controller			*/
									/*	o 0xFF = Have dynamically configured a	*/
									/*		 controller and structure	*/
									/*		  "dynamic_config" is init'd	*/

	struct cntrl_cfig    		dynamic_cnfg_struct ;		/*Parameters if ISA Controller Dynamically	*/
									/*configured.					*/
									/*NOTE: o Structure only valid if 		*/
									/*	  "dynamic_cnfg_cmplt" = 0xFF		*/
									/*	o Element "base_port" in structure is	*/
									/*	  ignored and therefore is assumed 0	*/
extern spinlock_t eqnx_mem_hole_lock;

/********************
 * Static Data Area *
 ********************/

/* Flag indicating if ROM Array below has been          */
/*initialized/built:					*/
/*	o 0x00 = Array not initialized			*/
/*	o 0xFF = Array initialized			*/

static  unsigned char		rom_array_built = 0;		

/*Byte mapped array of memory areas occupied by ROM	*/
/*NOTE: o Initialized by "FIND 16k MEMORY HOLE" function*/
/*	o Element 0 = ROM_AREA_BASE			*/
/*	o Each element/index = 0x800 (2k)		*/
/*	o Last element = ROM_AREA_LMT			*/
/*	o Element value 0x00 = No ROM			*/
/*	o Element value 0xFF = ROM present		*/
static	unsigned char		rom_array[MAX_ROM_LOCS] ;	


/************************************************************************************/
static unsigned char determ_isa_cnfg( unsigned short int base_port, 
         struct isa_cnfg_parms *ret_parms, unsigned char ssp_type); 
static unsigned char    determine_bus_interface  (unsigned short int,
unsigned long int, unsigned char *, unsigned char) ;

/*Find 16k Memory Hole Function		*/
static unsigned char	find_memory_hole (unsigned char, unsigned long int *);
static unsigned char occupied_check (unsigned long int physc_base_addr );
static unsigned char passive_occupied_chk (unsigned long int physc_addr,
	unsigned char *log_addr);
static unsigned char	rom_check (void) ;

/************************************************************************************/
static unsigned char determ_isa_cnfg( unsigned short int base_port, 
         struct isa_cnfg_parms *ret_parms, unsigned char ssp_type)
{
        volatile struct cntrl_cfig      *found_config_ptr = NULL ;
        unsigned long int               physc_addr ;
        unsigned long int               page_base ;
        unsigned int                    mem_index, i ;
        unsigned char                   vga_rom_found;
        unsigned int                    oride_index ;
        unsigned char                   uchar_ret_val;

        ret_parms->cntrlr_parms.base_port = base_port;
        ret_parms->method = 3;
        if ( (oper_input.sys_bus_type != 0x01) &&
             (oper_input.sys_bus_type != 0x02)){
                printk("bustype is not isa or eisa\n");
                ret_parms->status = 0x02 ;
                return (ret_parms->status) ;
        }
        if ( (ssp_type != IFNS_SSP) &&
                (ssp_type != IFNS_SSP4) ){
                printk("ssp type is not valid\n");
                ret_parms->status = 0x02 ;
                return (ret_parms->status) ;
        }
	
/*Set up & determine Controller's settings (parms.) as follows:								*/
/*	1. Check for Controller's I/O port in explicit table (see Operator Parameter structure)				*/
/*	2. Check defaut entry in Operator Parameter structure								*/
/*	3. Check if an explicit entry exists that is between 640k & 1M that is paged					*/
/*	4. Check if an explicit entry exists that is paged								*/
/*	5. Check if a configuration has been prevously dynamically determined						*/
/*NOTE: If none of the above are found then attempt to dynamically identify parameters for callers controller		*/

	for ( i = 0; i < oper_input.isa_explct_ents; ++i )	/*Search for caller's Controller in Explicit tbl	*/
	{
		if ( oper_input.isa_explct_tbl[i].base_port == base_port ) /*Did operator explicitly config controller ?*/
		{							   /*If yes, pt to entry			*/
			found_config_ptr = &(oper_input.isa_explct_tbl[i]) ;   	/*Set up ptr to Controller's parms	*/

		/*Common exit point - Controller's configuration parms identified					*/
		/*NOTE: found_config_ptr points to parms								*/
EXIT:
			ret_parms->cntrlr_parms.cntrlr_base_addr  = found_config_ptr->cntrlr_base_addr ;
			ret_parms->cntrlr_parms.cntrlr_mode       = found_config_ptr->cntrlr_mode ;
			if ( found_config_ptr->cntrlr_mode == 0x40 )	   /*Is Controller configured in Flat mode ?	*/
				ret_parms->cntrlr_parms.cntrlr_bus_inface = 0x20 ;	/*Yes, bus interface = 16 bits	*/
			else						   		/*No, Controller in page mode	*/
			{
				if ( found_config_ptr->cntrlr_bus_inface != 0xFF )	/*Determ interface i.e. dynamic?*/
				{							/*If no, return interface	*/
					ret_parms->cntrlr_parms.cntrlr_bus_inface = found_config_ptr->cntrlr_bus_inface ;
				}
				else							/*If yes, go dynamically determ	*/
				{							/*bus interface			*/
					uchar_ret_val = determine_bus_interface (base_port,	/*Determ Ctrlr's intrfce*/
								 	     found_config_ptr->cntrlr_base_addr,
								 	     &(ret_parms->cntrlr_parms.cntrlr_bus_inface),
/*RV8*/                      ssp_type);
					if ( uchar_ret_val != 0x00 )			/*Successful determination?	*/
					{						/*If no, ret with error 	*/
					ret_parms->status = uchar_ret_val ;
					return (ret_parms->status) ;
					}
				}
			}
			ret_parms->status = 0x00 ;
			return (ret_parms->status) ;				/*Return to caller with found status	*/
		}							/*End of found cntrlr's entry in explicit tbl	*/
	}							/*End of explicit table search (for loop)		*/



	if ( oper_input.isa_def_specd == 0xFF )			/*Did operator specify default controller parms ?	*/
	{							/*If yes, pt to entry					*/
		found_config_ptr = &(oper_input.isa_def_config) ;	/*Set up ptr to Controller's parms		*/
		goto EXIT ;						/*Go return to caller with parms		*/
	}
        /*Clear variable indicating no paged entry found*/
        found_config_ptr = (struct cntrl_cfig *)NULL;
	for ( i = 0; i < oper_input.isa_explct_ents; ++i )	/*Search for implicit parms by checking Explicit table	*/
	{							/*for paged entry that is between 640 & 1M		*/
		if ( oper_input.isa_explct_tbl[i].cntrlr_mode == 0x00 ) /*Does explicit entry describe a 16k area ?	*/
		{						/*If yes, use as default for non-specified controllers	*/
			if ( (oper_input.isa_explct_tbl[i].cntrlr_base_addr >= 0x0A0000l) &&
			     (oper_input.isa_explct_tbl[i].cntrlr_base_addr <  0x100000l)    )
			{						/*If found paged entry between 640k & 1M	*/
				found_config_ptr = &(oper_input.isa_explct_tbl[i]) ;   	/*Set up ptr Controller's config*/
				goto EXIT ;						/*Go return with parms		*/
			}
			if ( found_config_ptr == (struct cntrl_cfig *)0 )		/*Have already identified entry?*/
				found_config_ptr = &(oper_input.isa_explct_tbl[i]) ;	/*If no, save in case can't find*/
											/*entry betwn 640k & 1M		*/
		}							/*End of found cntrlr's entry in explicit tbl	*/
	}							/*End of implicit search  (for loop)			*/
	if ( found_config_ptr != (struct cntrl_cfig *)0 )	/*Did find at least find an explicit paged entry ?	*/
		goto EXIT ;					/*If yes, use it					*/
	ret_parms->method = 2;
	if ( dynamic_cnfg_init == 0xFF )			/*Has a set of dynamic parms been previously extablish ?*/
	{							/*If yes, set caller controller's parms the same	*/
		found_config_ptr = &dynamic_cnfg_struct ;		/*Set up ptr to Controller's parms		*/
		goto EXIT ;						/*Go return to caller with parms		*/
	}

/*As result of searching Operator/Installation explicit Controller parameters, default parameters, and previously 	*/
/*configured Controller dynamic parameters, have not found a suitable set of parms for caller's controller, therefore	*/
/*set up & DYNAMICALLY determine controller parms by searching address tables for an available 16k memory hole		*/

        /*Identify avail 16k mem areas*/
        uchar_ret_val = find_memory_hole(oper_input.sys_bus_type, &page_base); 
        if ( uchar_ret_val != 0 ){
                /*If no, return to caller indicating unable to config.  */
                printk("could'nt find any memory hole\n");
                ret_parms->status = uchar_ret_val ;
                return (ret_parms->status); /*Return error status to caller*/
        }
        for (mem_index = 0 ; ist.sys_isa_hole_status[mem_index] != 0;
             ++mem_index ){
		unsigned char cntlr_bus_inface;

                if ( ist.sys_isa_hole_status[mem_index] == 0xF0 ){
                        physc_addr = ist.sys_isa_holes[mem_index];
                        /*Get area's bus interface size         */
                        uchar_ret_val = determine_bus_interface (base_port,
                                physc_addr, 
                                &cntlr_bus_inface, 
                                ssp_type);
                        /*Bus interface determation successful?         */
                        if ( uchar_ret_val != 0x00)
                                /*If no, mark mem area unavail  */
                                /*and try next memory area      */
                                ist.sys_isa_hole_status[mem_index] = 
					uchar_ret_val;
			else{
                                if (cntlr_bus_inface == 0x20)
                                        ist.sys_isa_hole_status[mem_index] = 	
						0xF3 ;
				else
                      			/*16k memory area functional 
                      			w/8 bit bus intface*/
                      			/*Mem avail w/8 bit intface     */
                                        ist.sys_isa_hole_status[mem_index] = 	
						0xF1;
			}
		}
	}
        /*Loop searching for 16k page of unoccupied memory that */
        /*can be used with a 16 bit bus interface               */
        for (mem_index = 0 ; ist.sys_isa_hole_status[mem_index] != 0;
             ++mem_index ){                                                     
                /*Is 16k mem area (addr) available ?            */
                if ( ist.sys_isa_hole_status[mem_index] == 0xF3 ){

                        ist.sys_isa_hole_status[mem_index] = 0xFF ;
                	dynamic_cnfg_struct.cntrlr_bus_inface = 0x20;

                        /*Mem accessable w/16 bit inerface?*/
                        /*If yes, set up dynamic config */
                        /*parms for future use          */
       
                        /*Null base port                */
                        dynamic_cnfg_struct.base_port = 0x00 ;
                        /*Dynamic 16k mem area addr     */
                        dynamic_cnfg_struct.cntrlr_base_addr = 
                        	ist.sys_isa_holes[mem_index];
                        /*Dynamic 16k mem mode = pg     */
                        dynamic_cnfg_struct.cntrlr_mode = 0x00 ;
                        /*Bus interface est. via fn     */
                        /*Indicate Dynam Cnfg init'd*/
                        dynamic_cnfg_init = 0xFF ;                          
                        /*Indicate mem area alloc'd     */
                        /*Ptr to Cntrlr's parms */
                        found_config_ptr = &dynamic_cnfg_struct ;           
                        /*Go return with parms  */
                        goto EXIT ;                                         
                 }       /*End of mem accessable w/16 bit        */
        }/*End of "for" loop srch'g for avail mem area w/16 bit intrfce  */

        /*Did not successfully find an avail 16k mem area with a 16 bit bus interface, 
                check if should attempt VGA ROM 8-bit   */
        /*region (memory area) override                                                                                         */
        /*NOTE: - Any available memory holes at this point should have an 8 bit interface                                       */
        /*Should attempt to override 8 bit VGA ROM Memory ?             */

        if ( oper_input.vga_hole_override != 0xFF){
                /*If yes */
                /*Loop searching for VGA ROM at 0xC0000                 */
                for (mem_index = 0, vga_rom_found = 0 , 
                        oride_index = 0xFFFFFFFF ;
                        ( (physc_addr = ist.sys_isa_holes[mem_index]) != 0x00 ) ;
                        ++mem_index ){
                        if ( ((physc_addr & 0xFFFFF) >= 0xC0000) &&     /*Is table entry in VGA ROM 128k region ?       */
                                ((physc_addr & 0xFFFFF) <  0xE0000) ){
                                /*If yes, chk if hole = 8 bit RAM or VGA ROM    */
                                if ( (physc_addr & 0xFFFFF) == 0xC0000){
                                        /*Is mem area = VGA ROM addr space ?    */
                                        /*If yes, check if VGA ROM present      */
                                        /*If VGA ROM not present,       */
                                        /*Don't attempt override        */
                                        if ( ist.sys_isa_hole_status[mem_index] != 0x01 ) 
                                                break ;                                   
                                        /*Indicate system has VGA ROM           */
                                        vga_rom_found = 0xFF ;                  
                                }
                                else{
                                        /*Memory area not VGA ROM, validate     */
                                        /* some other ROM or 8 bit hole         */
                                        if (ist.sys_isa_hole_status[mem_index] != 0x01){
                                                /*Is memory area = ROM?         */
                                                /*If no, chk if 8 bit hole      */
                                                /*Is mem area = 8 bit   */ 
                                                if ( ist.sys_isa_hole_status[mem_index] == 0xF1){
                                                        /*If yes                */
                                                        /*Is Override addr estab?*/
                                                        if ( oride_index == 0xFFFFFFFF )
                                                                oride_index = mem_index ;       
                                                        /*If no                 */
                                                }
                                                else{
                                                        /*Mem area not ROM & not*/
                                                        /*an 8 bit hole, don't  */
                                                        /*attempt override      */
                                                        oride_index = 0xFFFFFFFF;               
                                                        break ;
                                                }
                                        }
                                }
                        }/*End of IF used to chk holes in VGA ROM region */
                }/*End of VGA ROM 8-bit hole area override FOR loop     */

                /*Did successfully find a 16k memory hole in VGA ROM    */
                /* region that can be used in 16 bit mode thus          */
                /*  overriding VGA ROM's 8 bit interface ??             */
                if ( (physc_addr == 0) && (vga_rom_found == 0xFF) &&            
                        (oride_index != 0xFFFFFFFF)){
                        /*If yes, use 16k memory area indicating override       */
                        /*Indicate mem area alloc'd     */
                        ist.sys_isa_hole_status[oride_index] = 0xFD ;     
                        /*Set base port = null          */
                        dynamic_cnfg_struct.base_port         = 0x00 ;                  
                        /*16k mem area addr     */
                        dynamic_cnfg_struct.cntrlr_base_addr  = 
                                ist.sys_isa_holes[oride_index]; 
                        /*16k mem mode = pg     */
                        dynamic_cnfg_struct.cntrlr_mode = 0x00;                         
                        /*Bus interfce = 16 bits*/
                        dynamic_cnfg_struct.cntrlr_bus_inface = 0x20 ;                          
                        /*Dynam Cnfg initialized*/
                        dynamic_cnfg_init = 0xFF ;                                              
                        /*Ptr to Cntrlr's parms */
                        found_config_ptr = &dynamic_cnfg_struct ;                               
                        /*Go return with parms          */
                        goto EXIT ;                                                             
                }
        }       /*End of VGA ROM 8-bit memory area override check (IF) */
/*Did not successfully find an avail 16k mem area with a 16 bit bus interface, check if an 16k mem area exists with an	*/
/*8 bit bus interface													*/
	for (mem_index = 0 ;					/*Loop searching for 16k page of unoccupied memory that	*/
	     ( ist.sys_isa_hole_status[mem_index] != 0xF1 ) &&	/*can be used with an 8 bit bus interface		*/
	     ( ist.sys_isa_hole_status[mem_index] != 0x00 ) ;
	     ++mem_index ) ;
	if ( ist.sys_isa_hole_status[mem_index] == 0xF1 )	/*Did find a memory area w/8 bit bus interface ?	*/
	{							/*If yes, use area					*/
		ist.sys_isa_hole_status[mem_index] = 0xFE ; 	    			/*Indicate mem area alloc'd	*/
		dynamic_cnfg_struct.base_port         = 0x00 ;				/*Set base port = null		*/
		dynamic_cnfg_struct.cntrlr_base_addr  = ist.sys_isa_holes[mem_index]; 	/*Dynamic 16k mem area addr	*/
		dynamic_cnfg_struct.cntrlr_mode       = 0x00 ;	    			/*Dynamic 16k mem mode = pg	*/
		dynamic_cnfg_struct.cntrlr_bus_inface = 0x00 ;				/*Bus interface = 8 bits	*/
		dynamic_cnfg_init = 0xFF ;			    			/*Indicate Dynam Cnfg init'd	*/
		found_config_ptr = &dynamic_cnfg_struct ;	    			/*Ptr to Cntrlr's parms		*/
		goto EXIT ;					    			/*Go return with parms		*/
	}

/*Did not successfully find an avail 16k mem area with an 8 bit bus interface, return to caller with not found status	*/
	ret_parms->status = 0xFF ;
	return (ret_parms->status) ;
}

/****************************************************************************************
 *                                                                              	*
 *				DETERMINE BUS INTERFACE SIZE				*
 *                                                                              	*
 * PURPOSE: For a given 16k memory area, determine its bus interface size (8/16 bits)	*
 *											*
 * NOTES: 										*
 *											*
 * CALL:  unsigned char    determine_bus_interface  (unsigned short int	 base_port,	*
 *						     unsigned long int	 physc_addr,	*
 *						     unsigned char	 *size      )	*
 * \*RV8*\         unsigned char   ssp_type)      *
*											*
 *			where: base_port  = Controller's base I/O port			*
 *			       physc_addr = Physical addr of 16 area			*
 *			       *size      = Pointer for bus interface size return parm:	*
 *						o 0x00 =  8 bit bus interface		*
 *						o 0x20 = 16 bit bus interface		*
 *											*
 *											*
 * RETURN:  - Status (unsigned char):							*
 *			o 0x00 = Function successfully determined memory areas bus	*
 *				 interface size ("size" return parameter valid)		*
 *			o 0x03 = Function unsuccessful due to memory area appearing to	*
 *				 be cached ("size" return parameter invalid)		*
 *			o 0x04 = Function unsuccessful due to a conflict, e.g. can not 	*
 *				 successfully read ICP's "channel counter" or bad base	*
 *				 I/O port ("size" return parm invalid).			*
 *			o 0x80 = Function unsuccessful due to memory mapping failure	*
 *				 ("size" return parameter invalid)			*
 *											*/
static unsigned char    determine_bus_interface  (unsigned short int   base_port,
                unsigned long int physc_addr, unsigned char     *size , 
                unsigned char ssp_type)

{
        /*Variables used to determine bus interface size */
        /*Logical base addr of 16k memory area (ICP ptr)        */
        volatile struct icp_gbl_struct        *log_icp_ptr ;          
        /*Mod 128k physical base addr that caller's memory area */
        unsigned long int               share_area_addr ;       
        /*resides in 16 bit read of ICP ATTN & Chan Ctr regs            */
        unsigned short int              attn_chan ;
        /*16 bit rd of ICP ATTN/Chan Ctr regs used for chan chnge*/
        unsigned short int              attn_chan_chng ;        

        /*General purpose variables */
        unsigned char                   uchar_ret_val ; 
        unsigned char                   return_status ;
        unsigned int i;
        void *  caddr;
        int ii; 
        /*Logical base addr of 16k memory area (SSP4 ptr)*/
        volatile struct ssp2_global_s  *log_ssp2_ptr ;         
        unsigned short int              cur_chnl;
        unsigned short int w;
        log_ssp2_ptr = NULL; /* get rid of compiler warning */
        log_icp_ptr = NULL;  /* get rid of compiler warning */

        if ( ssp_type == IFNS_SSP )
                physc_addr += ICP_GLOBAL_DISPLC;
		
#if (EQNX_VERSION_CODE < 131328)
	if (physc_addr > 0x100000){
#endif
		if ((caddr = ioremap(physc_addr, 0x1000)) == (void *) NULL){
                	printk("EQUINOX: determine_bus_interface: Failed to map physical address space = %x\n",
                        (unsigned int) physc_addr); 
                	return((unsigned char)0x80);
        	}
#if (EQNX_VERSION_CODE < 131328)
	}else
		caddr = (void *) physc_addr;
#endif		
        /*Did successfully map address ?*/
        /*If no, exit with error status*/
        if ( !caddr )
                return ((unsigned char)0x80) ;

        if ( ssp_type == IFNS_SSP ) {
                log_icp_ptr = (volatile struct icp_gbl_struct *) caddr;
        /*Disable ICP window in case enabled                    */
        outw( 0x0000, base_port + ISA_PAGE_SEL ) ;

        /*Port 1 = pg mode + 8 bit bus intface + addr bits 23 22*/
        /*Port 0 = addr bits 21 - 14                          */
        /*Init. Controller in page mode at caller's physc addr  */
        outw( ( ((physc_addr & 0xC00000) >> (22-8)) + 0x0000) |
                 ((physc_addr & 0x3FC000) >> 14),
                base_port + ISA_ADDR_LOW);

        /*Enable ICP window*/
        outw( 0x100, base_port + ISA_PAGE_SEL ) ;
        /*Set ICP = Dram steer + 8 bit bus                      */
        log_icp_ptr->gicp_bus_ctrl = 0x08 ;

        /* TEMP for fixed hardware condition */
        for ( ii = 512; ii != 0; --ii )
                if ( log_icp_ptr->gicp_bus_ctrl == 0x08 )
                  break;
        if ( ii == 0 ) {
          return_status = 4;
          goto DETERMINE_BUS_EXIT;
        }

    /*determine if area functional with 8 bit bus interface*/
    /*NOTE: Controller & ICP set for 8 bit interface*/

        /*Disable addressing, so can't find in 128k srch        */
        outw( 0x0000,base_port + ISA_PAGE_SEL) ;
        /*Base of 128k region caller's address resides in*/
        /*Set up to determine if 128k region that caller's      */
        /*mem area resides in is occupied with some other memory*/
        share_area_addr = physc_addr & 0xFFFE0000 ;
        for (i = 0 ; (i < 8) &&
             ( occupied_check(share_area_addr+(i*0x4000) ) == 0xF0) ;
             ++i        ) ;
        /*Enable ICP addressing again                           */
        outw( 0x100, base_port + ISA_PAGE_SEL) ;
/*-------------------------------------------*/
 }
 else  /* IFNS_SSP4 */
 {
   log_ssp2_ptr = (volatile struct ssp2_global_s *) ((unsigned char *)caddr + 0x400);

   /*Disable ICP window, set paged and memory-mapped*/
   outb( 0x00, base_port + SSP4_PAGE_SEL ) ;

   /*addr bits 14 - 15, disable ints */
   /*Init. Controller in page mode at caller's physc a ddr  */
   outb((((physc_addr & 0x0000C000) >> 8) & 0xFF),
          (base_port + SSP4_ADDR_LOW));


   /*Port 0 = addr bits 21 - 14 */
   /*Init. Controller in page mode at caller's physc addr  */
   outb(  (((physc_addr & 0xFFFF0000) >> 16) & 0xFF),
          (base_port + SSP4_ADDR_HIGH));

   /*Enable ICP window, base ICP*/
   outb( 0x01, (base_port + SSP4_PAGE_SEL));
   /*Set ICP = 8 bit bus*/
   log_ssp2_ptr->bus_cntrl = 0;

 /*determine if area functional with 8 bit bus interface*/
 /*NOTE: Controller & ICP set for 8 bit interface
    */
   /*Disable addressing, so can't find in 128k srch */
   outb(0x00, (base_port + SSP4_PAGE_SEL)) ;
   /*Base of 128k region caller's address resides in */
   /*Set up to determine if 128k region that caller's*/
   /*mem area resides in is occupied with some other memory*/
   share_area_addr = physc_addr & 0xFFFE0000 ;
   for (i = 0 ; (i < 8) &&
        ( occupied_check(share_area_addr+(i*0x4000) ) == 0xF0) ;
        ++i        ) ;
   /*Enable ICP addressing again*/
   outb( 0x01, (base_port + SSP4_PAGE_SEL));
 }

/*-------------------------------------------*/
        /*Is any other mem present in 128k addr space ?         */
        /*If no, caller's mem area can use 16 bit bus interface */
        /*Set up bus interface return parm = 16 bit     */
        /*Set up function's return status               */
        /*Return to caller*/
        if ( i >= 8 ){
                *size = 0x20 ;
                return_status = 0x00 ;
                goto DETERMINE_BUS_EXIT ;
        }


        /* is it system ROM? */
        if ( share_area_addr == 0xe0000 ) {
                /* is 0xe0000 to 0xeffff free? */
                if ( i >= 4 ){
                /* yes - set to 16 bits */
                /*Set up bus interface return parm = 16 bit     */
                /*Set up function's return status               */
                /*Return to caller                              */
                *size = 0x20 ;
                return_status = 0x00 ;
                goto DETERMINE_BUS_EXIT ;
                }       
        }

    /*16k memory area resides in a 128k memory region that is not empty, determine if 128k region is occupied by memory */
    /*using an 8 bit bus interface.  Perform this check by issuing a 16 bit read operation with Controller & ICP in     */
    /*8 bit mode.                                                                                                       */

 if ( ssp_type == IFNS_SSP ){

        /*Issue 16 bit mem read of ICP regs             */
        attn_chan = *((unsigned short int *)(&log_icp_ptr->gicp_attn));
        /*Does 16 bit read appear to have functioned ?  */
        if ( ((attn_chan & 0xC0F0) == 0) &&
            ((*((unsigned short int *)(&log_icp_ptr->gicp_attn)))&0xC0FF) == (attn_chan & 0xC0FF))
        {
            /*If yes, validate can "see" ICP's chan ctr ticking  */
            /*NOTE: Chan ctr is hi byte of 16 bit read           */
                for (i = ((unsigned long int)((260000*2)/(10*8))) ;
                     i != 0 ; --i ) {
                        /*ICP's chan ctr in hi byte*/
                        attn_chan_chng = *((unsigned short int *)(&log_icp_ptr->gicp_attn));
                        /*Did val chng frm 1st rd? */
                        if ( (attn_chan_chng & 0x3F00) != (attn_chan & 0x3F00) )
                                /*If yes, 8 bit mode ok    */
                                break ;
                }
                /*Did successfully read 16 bits in 8 bit mode?       */
                if ( i != 0){
                    /*If yes, implies other memory in same 128k region is*/
                    /*functioning with an 8 bit bus interface            */
                        /*Set up bus interface return parm = 8 bit      */
                        /*Set up function's return status               */
                        /*Return to caller                              */
                        *size = 0x00 ;
                        return_status = 0x00 ;
                        goto DETERMINE_BUS_EXIT ;
                }
        }

  }
        else  /* IFNS_SSP4 */
  {
        /*Issue 16 bit mem read of ICP regs */
       
          w = * ((unsigned short int *) (&log_ssp2_ptr->filler3));

          if ( w == 0x0100 ){
          /* is gssp4_rev == 1 when read in 8-bit mode? */
          /* Issue 16 bit mem read */
             cur_chnl = * ((unsigned short int *) (&log_ssp2_ptr->unused_3)); 
            
             if ( !(cur_chnl & 0xc000) ) {
                 /*If yes, validate can "see" ICP's chan ctr ticking  */
                /*NOTE: Chan ctr is hi byte of 16 bit read */
                for (i = ((unsigned long int)((260000*2)/(10*8))) ;
                   i != 0 ;
                   --i ) {
                    /*ICP's chan ctr in hi byte */
                   w = * ((unsigned short int *) (&log_ssp2_ptr->unused_3)); 
                   /*Did val chng frm 1st rd? */
                   if ( (w & 0x3F00) != (cur_chnl & 0x3F00) )
                            /*If yes, 8 bit mode of */
                            break;
                 }
                 /*Did successfully read 16 bits in 8 bit mode ?*/
                 /*If yes, implies other memory in same 128k region is */
                        /*functioning with an 8 bit bus interface */
                 if ( i != 0 ){
                      *size = 0x00 ; 
                        /*Set up bus interface return parm = 8 bit */
                    /*Set up function's return status */
                    /*Return to caller */
                    return_status = 0x00 ;
                    goto DETERMINE_BUS_EXIT ;

                 }
             }
     }
   }
 


    /*16 byte read of ICP's Channel Counter & Attention registers failed indicating that caller's 16k memory area       */
    /*resides in a 128k region with memory using a 16 bit bus interface.  Return to caller indicating 16k memory area   */
    /*requires a 16 bit bus interface                                                                                   */
        *size = 0x20 ;                                          /*Set up bus interface return parm = 16 bit             */
        return_status = 0x00 ;                                  /*Set up function's return status                       */



    /*Function's exit point, disable Controller & free up memory area                                                   */
    /*NOTE: return_status = function's return status parameter                                                          */

DETERMINE_BUS_EXIT:
 if ( ssp_type == IFNS_SSP ){

        if ( return_status != 4 )
          log_icp_ptr->gicp_bus_ctrl = 0;

        /*Disable Controller's addressing                       */
        outw( 0x0000, base_port + ISA_PAGE_SEL ) ;
        outw( 0x0000, base_port + ISA_ADDR_LOW) ;
#if (EQNX_VERSION_CODE < 131328)
	if (physc_addr > 0x100000)
#endif
	  iounmap(caddr);
 }
 else  /* IFNS_SSP4 */
 {
   if ( return_status != 4 )
     log_ssp2_ptr->bus_cntrl = 0;

   /*Disable Controller's addressing*/
   outb(0x00, (base_port + SSP4_PAGE_SEL) ) ;
   /* clear physical addr */
   outb(0, (base_port + SSP4_ADDR_LOW) );
   /* clear physical addr */
   outb(0,(base_port + SSP4_ADDR_HIGH) );
#if (EQNX_VERSION_CODE < 131328)
	if (physc_addr > 0x100000)
#endif
	  iounmap(caddr);
 }


        /* no return code */
        uchar_ret_val = 0;
        /*Did successfully release memory area ?                */
        /*If no, indicate failure reason                        */
        if ( uchar_ret_val != 0 )
                return_status = 0x80 ;
        return (return_status) ;

}  /* determine_bus_interface */
/****************************************************************************************
 *                                                                              	*
 *				FIND 16k MEMORY HOLE					*
 *                                                                              	*
 * PURPOSE: To indentify 16k unused areas of memory (memory hole) on an ISA or EISA	*
 *	    system and build arrays ("sys_isa_holes" and "sys_isa_hole_status" in 	*
 *	    structure "ist_struct") indicating results of identification.  		*
 *											*
 * NOTES: o Function unconditionally DISABLES/ENABLES INTERRUPTS !!!!!!			*
 *											*
 * CALL:  unsigned char    find_memory_hole  (unsigned char 	   bus_type,		*
 *					      unsigned long int	   *avail_addr)		*
 *											*
 *			where: bus_type   = System bus type:				*
 *						o 1 = ISA (perform bit 31 testing)	*
 *						o 2 = EISA (don't perform bit 31 testng)*
 *						o ? = Any other value, function returns	*
 *						      immediately with value of 0x03	*
 *			       avail_addr = Ptr to location that function uses to return*
 *					    32 bit physical addr of unoccupied 16k area	*
 *					    (i.e. physical addr of a found memory hole)	*
 *											*
 *											*
 * RETURN:  - Status (unsigned char):							*
 *			o 0x00 = Function successfully found memory hole with		*
 *				 NOTE: "avail_addr" = 32 bit physical addr of available	*
 *					memory area					*
 *			o 0x02 = Unrecognized sytem bus parameter ("bus_type" invalid)	*
 *				 ("avail_addr" = 0).					*
 *			o 0x80 = Function unsuccessful due to not being able to map	*
 *				 addresses ("avail_addr" = 0).				*
 *			o 0xFF = No available memory holes ("avail_addr" = 0).		*
 *	    - Arrays "sys_isa_holes" and "sys_isa_hole_status" in structure "ist_struct"*
 *	      (see ist.h) are built and returned in "ist" area				*
 *											*
 ****************************************************************************************/

static unsigned char	find_memory_hole (unsigned char bus_type,
		  	  unsigned long int   *avail_addr )

{

/*Adrress used to test system's bit 31 addressing */
#define	B31_PHYSC_ADDR	0x80000000				
/*Physc addr 0, used to test system's bit 31 addressing	*/
#define	PHYSC_ADDR_0	0x0					


/*Variables used to probe for memory holes */
	unsigned int pa_index ; /*Probe array index*/

/*Variables used to check if system supports bit 31 addressing*/

/*Logical addr used to access physc. addr w/bit 31 set	*/
	unsigned long int	*log_bit_31_addr ;	
/*Logical addr used to access physc. addr 0		*/
	unsigned long int	*log_0_addr ;		
	/*Flag indicating if system supports bit 31 addressing	*/
	/*	- 0x00 = not supported				*/
	/*	- 0xFF = supported				*/
	unsigned char		bit_31_support ;		
	/*Displacement to area that is not all 0's or 1's	*/
	unsigned long int	non_1s_displc ;			
	/*Saved contents of physical location w/bit 31 set	*/
	unsigned long int	physc_hi_sav_val ;		
	/*Saved contents of physical location 0			*/
	unsigned long int	physc_lo_sv_val ;		
	/*Contents of physical location 0			*/
	unsigned long int	physc_lo_val ;			

	/*General purpose variables*/
	unsigned int		i ;
	unsigned int		j ;
	unsigned long   flags;

	volatile void *			caddr;


	/*Is bus type parameter as expected ?			*/
	if ( (bus_type != 1) && (bus_type != 2) ){							
		/*If no, exit indicating did not find hole		*/
		/*Set returned mem hole addr to null			*/
		*avail_addr = 0 ;				
		/*Return to call with bad parm status			*/
		return ((unsigned char)0x02) ;			
	}

	/*Backgound Table of Memory Hole addresses to probe for availability*/
	/*Initialize Memory Hole address array to all 0's	*/
	/*Set Memory Hole address = null (end of table)		*/
	for ( i = 0; i < HOLE_TABLE_SIZE; ++i )			
		ist.sys_isa_holes[i] = 0 ;			

	/*Backgound Status Table (holds results of memory hole probe)*/
	/*Initialize Memory Hole Status array to all 0's	*/
	/*Set corresponding Memory Hole status = not checked	*/
	for ( i = 0; i < HOLE_TABLE_SIZE; ++i )			
		ist.sys_isa_hole_status[i] = 0 ;		


	/*Set up and probe for availability 16k areas between 640k & 1M								*/
	/*Loop to move probe addresses that are btw 640k & 1M	*/
	for ( i = 0; i < oper_input.isa_num_ents; ++i )	{
		/*Is index about to overflow table ?			*/
		/*If yes, leave last entry = null (0)			*/
		if ( i >= (HOLE_TABLE_SIZE-1) )			
			break ;					
		/*Move an addr to probe from "space.c" 640k-1M array*/
		ist.sys_isa_holes[i] = oper_input.isa_addr_tbl[i] ; 
	}
	/*Loop to probe areas & set status till exhaust array	*/
	/*If Null entry in table, search exhausted		*/
	for ( pa_index = 0 ;ist.sys_isa_holes[pa_index] != 0 ;++pa_index ){
		/*Should issue progress messages ?*/
		if ( !oper_input.quiet_mode ){						
       			printk("CHECK 16k ADDRESS SPACE %x FOR AVAILABILITY \n", 
				 (unsigned int) ist.sys_isa_holes[pa_index] ) ;
		}
		ist.sys_isa_hole_status[pa_index] = occupied_check (ist.sys_isa_holes[pa_index]) ;
	}

/*Completed searching all addresses between 640k & 1M, set up and 
determine if system supports bit 31 addressing
NOTE: Some ISA systems use a dual memory bus architecture such 
that when physical addr bit 31 is set, the low 24 bits
are sent out the ISA bus instead of system's local memory bus.*/

	/*Should probe mem addrs that have bit 31 set ?	*/
	/*If yes, ISA bus w/bit 31 chk enabled, chk if sys	*/
	/* supports bit 31 addressing scheme		*/
	if ( (bus_type == 1) && (oper_input.isa_bit_31_probe == 1) ) {

		if ((caddr = ioremap(B31_PHYSC_ADDR, 0x1000)) == (void *) NULL){
                	printk("EQUINOX: find_memory_hole: B31, Failed to map physical address space = %x\n",
                        (unsigned int) B31_PHYSC_ADDR); 
                	return((unsigned char)0x80);
        	}
		/*Did successfully map probe address ?		*/
		if ( !caddr ){
			/*If no, exit indicating did not find hole	*/
			/*Set returned mem hole addr to null		*/
			*avail_addr = 0 ;				
			/*Return to caller with error status		*/
			return ((unsigned char)0x80) ;			
		}
		else
		  log_bit_31_addr = (unsigned long int *) caddr;
		caddr = PHYSC_ADDR_0;
		if ( !caddr ){
			/*If no, exit indicating did not find hole	*/
			/*Set returned mem hole addr to null		*/
			*avail_addr = 0 ;				
			/*Return to caller with error status		*/
			return ((unsigned char)0x80) ;			
		}
		else
		  log_0_addr = (unsigned long int *) caddr;
		/*Init flag indicating bit 31 addr not supported*/
		bit_31_support = 0 ;					
		/*Detrm. displc to area in low mem that	*/
		/* is not all 0's or 1's		*/
		for ( non_1s_displc = 0; non_1s_displc < 8; ++non_1s_displc ){
			/*Does loc = all 0's or all 1's ?	*/
			/*If no, use displacement to test bit 31*/
			if ( (*(log_0_addr+non_1s_displc) != 0) &&		
			     (*(log_0_addr+non_1s_displc) != 0xFFFFFFFF ) )
				break ;						
		}
		/*Did find non-0's/1's location in low mem ?	*/
		if ( non_1s_displc < 8 ){
			/*If yes, test if bit 31 addressing functional ?*/
			/*Get 32 bit value @ physc addr 0*/
			physc_lo_sv_val  = *(log_0_addr+non_1s_displc) ;	
			/*Get val @ physc addr 0x80000000*/
			physc_hi_sav_val = *(log_bit_31_addr+non_1s_displc) ;	
			/*Did hi addr actually addr 0 ?	*/
			if ( physc_lo_sv_val != physc_hi_sav_val ){
				spin_lock_irqsave(&eqnx_mem_hole_lock, flags);
				/* clobber low memory		*/
				/*Write test pattern @ hi addr	*/
				*(log_bit_31_addr+non_1s_displc) = 0xEE11DD22 ;		
				/*Read physc 0*/
				physc_lo_val = *(log_0_addr+non_1s_displc) ;		

				/*Did test pat appear @0 or	*/
				/* did low mem get clobbered ?	*/
				if ( (physc_lo_val == 0xEE11DD22)    || 		
				     (physc_lo_val != physc_lo_sv_val) ){
					/*If yes, sys not support bit 31*/
					/*Restore value @0		*/
					*(log_0_addr+non_1s_displc) = physc_lo_sv_val ;	
					/*Enable interrupts		*/
       					spin_unlock_irqrestore(&eqnx_mem_hole_lock, flags); 
				}
				else {
					/*Restore bit value @ 0x80000000*/
				  *(log_bit_31_addr+non_1s_displc) = physc_hi_sav_val ;	
       				  spin_unlock_irqrestore(&eqnx_mem_hole_lock, flags); 
				  /*Indicate bit 31 addr supported*/
				  bit_31_support = 0xFF ;
				}
			}
			vfree(log_bit_31_addr);

			/*If System supports bit 31 ISA bus addressing 
			(dual memory addressing architecture), set up and probe for*/
			/*availability 16k areas with bit 31 set*/
			/*Does system support dual addr arch. ?	*/
			if ( bit_31_support == 0xFF ){
				/*If yes, bld tbls/arrays w/bit 31 addrs*/
				/*Add addrs to tbl*/
				for ( i = 0, j = pa_index; 
					i < oper_input.isa_num_bit_31_ents; ++i, ++j) {
					/*Is index about to overflow table ?	*/
					/*If yes, lv last entry = 0	*/
					if ( j >= (HOLE_TABLE_SIZE-1) )		
						break ;					
					ist.sys_isa_holes[j] = oper_input.isa_bit_31_tbl[i];
				}
				/*Loop to probe memory areas	*/
				for (; ist.sys_isa_holes[pa_index] != 0; ++pa_index ){
					/*Should issue progress msgs ?	*/
					if ( !oper_input.quiet_mode ) /*If yes*/
       						printk("31bit: CHECKING 16k ADDRESS SPACE %x FOR AVAILABILITY \n", 
						 (unsigned int) ist.sys_isa_holes[pa_index] ) ;
						ist.sys_isa_hole_status[pa_index] = 
							occupied_check (ist.sys_isa_holes[pa_index]) ;
				}
			}/*End of bit 31 adrress area probing	*/
		}/*End of bit 31 functionality testing		*/
	}/*End of ISA bit 31 check				*/
	if ( !oper_input.quiet_mode )/*Should issue progress msgs ?*/
          printk("\n" );/*If yes, position cursor at new line*/

	/*Memory table/array completed, determine if have identified 
	at least 1 available memory area/hole, and if so, return	*/
	/*address of 1st found entry in table.*/

	/*Search Status Table till:				*/
	/*	o Find an available area  OR			*/
	/*	o Exhaust table					*/
	for ( pa_index = 0 ;	
	      ist.sys_isa_hole_status[pa_index] != 0xF0 &&	
	      ist.sys_isa_hole_status[pa_index] != 0;		
	      ++pa_index ) ;
	/*Did find an unoccupied 16k area ?			*/
	if ( ist.sys_isa_hole_status[pa_index] == 0xF0 ){							
		/*If yes, return physical address to caller		*/
		/*Return physical address of unoccupied 16k area*/
		*avail_addr = ist.sys_isa_holes[pa_index];		
		/*Return to caller with "found" status		*/
		return ((unsigned char)0x00) ;				
	}

	/*Searched of entire Probe array failed to identify an available 
	16k memory region, return "no avail memory hole" status*/
	/*Set returned mem hole addr to null			*/
	*avail_addr = 0 ;					
	/*Return to caller with no avail mem hole error status	*/
	return ((unsigned char)0xFF) ;				
}  /* find_memory hole *//*END of FIND 16k MEMORY HOLE FUNCTION*/

/****************************************************************************************
 *                                                                              	*
 *			CHECK IF 16K ADDRESS SPACE OCCUPIED				*
 *                                                                              	*
 * PURPOSE: To determine if a 16k area of memory (ISA address space) is empty i.e. not	*
 *	    occupied by ROM/RAM or any other ISA device.				*
 *											*
 * NOTES: o Function unconditionally DISABLES/ENABLES INTERRUPTS !!!!!!			*
 *	  o Function MAY WRITE to memory area being checked				*
 *											*
 * CALL:  unsigned char occupied_check (unsigned long int	physc_base_addr) 	*
 *											*
 *			where: physc_base_addr = Physical base addr of 16k memory area	*
 *						 to check if occupied			*
 *											*
 *											*
 * RETURN:  - Status 16k area (unsigned char):						*
 *		o 0x01 = Address space occupied by ROM					*
 *		o 0x02 = Address space occupied by RAM					*
 *		o 0x80 = Could not map/de-map physical (probe) address			*
 *		o 0xF0 = Address space available					*
 *											*
 ****************************************************************************************/

static unsigned char occupied_check (unsigned long int physc_base_addr )

{

	/*Logical base address of memory area to probe		*/
	unsigned char *log_addr ;			
	/*General purpose variable used for return status	*/
	/*Function's return status parameter			*/
	unsigned char		return_status ;			
	/*Ctr used to srch for RAM within 16k memory space	*/
	unsigned int		srch_ctr ;			
	/*Save contents of memory 				*/
	unsigned long int	sv_mem_0 ;			
	unsigned long   flags = 0;
	/*Save contents of memory 				*/
	unsigned long int	sv_mem_1 ;			
	void * 			caddr;
	volatile unsigned int	*mem_rdp;
	volatile unsigned int	mem_rd;
	unsigned int		rd_ctr;
#ifdef DEBUG
	char prt_str[200];
#endif

#if (EQNX_VERSION_CODE < 131328)
	if (physc_base_addr > 0x100000){
#endif

		if ((caddr = ioremap(physc_base_addr, 0x4000)) == (void *) NULL){
               		printk("EQUINOX: occupied check: Failed to map physical address space = %x\n",
                       	(unsigned int) physc_base_addr); 
               		return((unsigned char)0x80);
        	}
#if (EQNX_VERSION_CODE < 131328)
	} else
		caddr = (void *) physc_base_addr;
#endif		

	if ( !caddr )/*Did successfully map probe address ?*/
	/*Return to caller with can't map physc addr error	*/        
		return ((unsigned char)0x80);
	else
		log_addr = (unsigned char *) caddr;
	/*Determine if mem area appears unoccupied	*/
	return_status = passive_occupied_chk (physc_base_addr, log_addr); 
#ifdef DEBUG
sprintf(prt_str,"after passive_occuped_check with params, phy addr = %x log addr = %x\n", 
	(unsigned int) physc_base_addr, (unsigned int) log_addr);
printk(prt_str);
#endif
	/*Is memory area occupied ?				*/
	if ( return_status == 0xF0 ){							
	/*If no, mem area does not appear occupied, attempt to	*/
	/*write & read area to assure not occupied		*/
		/*Loop to perform wrt/rd operations in 1k inc's	*/
		for ( srch_ctr = 0; srch_ctr < 16; ++srch_ctr )	{
			/*Read & Save location 0 and 1	*/
			sv_mem_0 = *((unsigned long int *)(log_addr+(1024*srch_ctr))) ;	 
			sv_mem_1 = *((unsigned long int *)(log_addr+(1024*srch_ctr)+4)); 
        		/*Disable interrupts in case	*/
			spin_lock_irqsave(&eqnx_mem_hole_lock, flags);
			/*write does clobber mem	*/
			/*Write location 0		*/
			*((unsigned long int *)(log_addr+(1024*srch_ctr))) =    	 
			  (unsigned long int)0xAAAA5555 ;
			/*Write location 4, set data	*/
			/*lines high			*/
			*((unsigned long int *)(log_addr+(1024*srch_ctr)+4)) =		 
			  (unsigned long int)0xFFFFFFFF ;		 	 	 
			
			for ( rd_ctr = 32; rd_ctr != 0; --rd_ctr )
			{
				mem_rdp = (volatile unsigned int *)(log_addr+(1024*srch_ctr));
				mem_rd = *mem_rdp;
				if ( mem_rd == (unsigned long int)0xFFFFFFFF )
			  	  break;
			}
			if ( rd_ctr == 0 ) {
				/*Yes, area occupied by RAM	*/
				/*Restore		*/
				*((unsigned long int *)(log_addr+(1024*srch_ctr))) = sv_mem_0 ;	 
				*((unsigned long int *)(log_addr+(1024*srch_ctr)+4)) = sv_mem_1; 
				spin_unlock_irqrestore(&eqnx_mem_hole_lock, 
						flags);
				return_status = ((unsigned char)0x02) ;	/*Return area occupied*/
				break ;
			}/*by RAM status	*/
			spin_unlock_irqrestore(&eqnx_mem_hole_lock, flags);
		}
	}/*End of write/read check loop				*/

    /*Determination has been made whether caller's memory area 
	is occupied, free memory & return status to caller	*/
#if (EQNX_VERSION_CODE < 131328)
	if (physc_base_addr > 0x100000)
#endif
		iounmap(caddr);
	return (return_status) ;/*Return status*/
}  /* occupied_check */


/****************************************************************************************
 *                                                                              	*
 *		     CHECK IF 16K ADDRESS SPACE OCCUPIED (NON_WRITE)			*
 *                                                                              	*
 * PURPOSE: To determine if a 16k area of memory (ISA address space) is empty i.e. not	*
 *	    occupied by ROM/RAM or any other ISA device by only reading memory.		*
 *											*
 * NOTES: o Function unconditionally DISABLES/ENABLES INTERRUPTS !!!!!!			*
 *	  o Function performs check by only searching ROM array & reading slected memory*
 *	    locations (in caller's address space).  Function DOES NOT WRITE to memory	*
 *	    area. 									*
 *											*
 * CALL:  unsigned char passive_occupied_chk (unsigned long int   physc_addr,		*
 *					      unsigned char	  *log_addr) 		*
 *											*
 *			where: physc_addr = Physical addr of 16k memory area to check	*
 *					    if occupied					*
 *			       log_addr   = Logical addr of 16k memory area to check if	*
 *					    occupied					*
 *											*
 *											*
 * RETURN:  - Status 16k area (unsigned char):						*
 *		o 0x01 = Address space occupied by ROM					*
 *		o 0x02 = Address space occupied by RAM					*
 *		o 0x80 = Could not map/de-map ROM area					*
 *		o 0xF0 = Address space not occupied by ROM & does not appear to be 	*
 *			 occupied by RAM						*
 *											*
 ****************************************************************************************/

static
unsigned char	passive_occupied_chk (unsigned long int	   physc_addr,
		      unsigned char *log_addr   )

{
	/*General purpose variable used for return status	*/
	unsigned char		uchar_ret_val ;			
	/*Prob addr's index into ROM array			*/
	unsigned int		rom_array_index ;		
	/*Ctr used to srch for ROM/RAM within 16k probe area	*/
	volatile unsigned int		srch_ctr ;			
	volatile unsigned int		*mem_rdp;
	volatile unsigned int		mem_rd;
	volatile unsigned int		rd_ctr;


	/*Did initialize ROM Array successfully ?	*/
	if ( (uchar_ret_val = rom_check ( )) != 0 )		
		/*If no, exit with error status			*/
		return ( uchar_ret_val ) ;			
	/* is addr to check in VGA graphic area? */
	if ( ((physc_addr & 0xfffff) >= 0xa0000) &&		
	     ((physc_addr & 0xfffff) < 0xb0000) ){
		rom_array_index = (0xc0000 - ROM_AREA_BASE)/2048;
		if ( rom_array[rom_array_index] != 0 )
			/* if yes, return arrea occupied by RAM */
			return( (unsigned char) 0x02 );		
	}
	/*Is addr to check in system ROM area ?			*/
	if ( ((physc_addr & 0xFFFFF) >= ROM_AREA_BASE) &&	
	     ((physc_addr & 0xFFFFF) <  ROM_AREA_LMT) ) {
		/*If yes, check if ROM occupies any portion of 16k area	*/
		/*Mem area's index into ROM array	*/
		rom_array_index = ((physc_addr & 0xFFFFF) - ROM_AREA_BASE)/2048 ; 
		/*Develop # ROM loc's in 16k spc		*/
		srch_ctr = MAX_ROM_LOCS - rom_array_index ;		
		if ( srch_ctr > 8 ) /*Is addr at "top" of ROM area ?		*/
			/*If no, then max # ROM locations to check	*/
			srch_ctr = 8 ;
		/*Loop thru ROM array chk'g if ROM occupies any	*/
		/* portion of caller's 16k memory area		*/
		for ( ; srch_ctr != 0; --srch_ctr ){							
			/*Is ROM present within area ?			*/
			if ( rom_array[rom_array_index++] != 0 )	
			/*If yes, return area occupied by ROM status 	*/
				return ((unsigned char)0x01) ;		
		}
	}/*End of ROM check loop					*/

/*Caller's 16k block is not occupied by ROM, 
set up and check if occupied by RAM*/
#ifdef DEBUG
printk("checking ram for address %x, logical addr = %x\n", 
	(unsigned int) physc_addr, (unsigned int) log_addr);
#endif
	/*Loop to perform read operations on area		*/
	for ( srch_ctr = 0; srch_ctr < 16; ++srch_ctr )	{
		for ( rd_ctr = 32; rd_ctr != 0; --rd_ctr ) {
			mem_rdp = (volatile unsigned int *)(log_addr+(1024*srch_ctr));
			mem_rd = *mem_rdp;
			if ( mem_rd == (unsigned long int) 0xffffffff )
			  break;
		}
		if ( rd_ctr == 0 )
		/*If no, Return area occupied by RAM	*/
			return ((unsigned char)0x02) ;	
	}

/*Caller's 16k block does not appear to be 
occupied by RAM, return status to caller*/
	return ((unsigned char)0xF0) ;

}  /* passive_occupied_chk */

/****************************************************************************************
 *                                                                              	*
 *				      ROM SEARCH					*
 *                                                                              	*
 * PURPOSE: To build byte mapped array of memory areas (address space) occupied by ROM.	*
 *											*
 * NOTES: 										*
 *											*
 * CALL:  unsigned char rom_check (void)						*
 *											*
 *											*
 * RETURN:  - Function status:								*
 *		o 0x00 = ROM array successfully built					*
 *		o 0x80 = Could not map/de-map physical (probe) address			*
 *											*
 ****************************************************************************************/

static unsigned char	rom_check (void) 
{
	/*Logical base address of ROM memory region		*/
	unsigned char  	*rom_probe_base ;		
	/*ROM array index					*/
	unsigned int		ra_index ;			
	/*Logical base address of memory area to probe		*/
	unsigned char  	*log_probe_addr ;		
	/*Size of a found ROM (in units of 2k)			*/
	unsigned short int	rom_size ;			
	void *caddr;

	/*Has ROM Array been previously built ?			*/
	if ( rom_array_built == 0xFF )				
	/*If yes, return to caller w/good status		*/
		return ((unsigned char)0) ;			

#if (EQNX_VERSION_CODE > 131327)
		if ((caddr = ioremap(ROM_AREA_BASE, (MAX_ROM_LOCS * ROM_BNDRY_SZ))) == (void *) NULL){
               		printk("EQUINOX: rom check: Failed to map physical address space = %x\n",
                       	(unsigned int) ROM_AREA_BASE); 
               		return((unsigned char)0x80);
        	} /* Pat */
#else
	caddr = (void *) ROM_AREA_BASE;
#endif	
	/*Did successfully map ROM area ?			*/
	if ( !caddr )						
	/*If no, exit indicating mapping failure	*/
		return ((unsigned char)0x80) ;				
	else
		rom_probe_base = (unsigned char *) caddr;
	/*Set up & initialize ROM array*/
	/*Set Up & srch all addresses where ROM might reside	*/
	for ( ra_index = 0; ra_index < MAX_ROM_LOCS; )		
	{
		/*Pt to nxt possible ROM probe addr		*/
		log_probe_addr = rom_probe_base+(ROM_BNDRY_SZ * ra_index); 
		/*Is ROM signature at probe address ?	*/
		if ( (*log_probe_addr == (unsigned char)0x55) && 	   
		     (*(log_probe_addr+1) == (unsigned char)0xAA) ) {

			/*If yes, found ROM signature			*/
			/*Get ROM's sz in 2k units & add another*/
			/* unit if not mod 2k			*/
			rom_size = *(log_probe_addr+2) >> 2 ;			
			if ( (*(log_probe_addr+2) & 0x03) != 0 ) ++rom_size; 	
			/*Is number of blocks sane ?			*/
			if ( rom_size != 0 ){						
			/*If yes, then found ROM, update ROM array	*/
				for ( ; (rom_size != 0) && (ra_index < MAX_ROM_LOCS); --rom_size)
				/*Indicate ROM present in 2k area	*/
					rom_array[ra_index++] = 0xFF ;		
			}
			else 
			/*Found ROM signature but # blocks bad, continue*/
			/* as if no ROM found				*/
				rom_array[ra_index++] = 0 ;		
		}/*End of logic if ROM				*/
		else /*ROM signature not found at Probe addr		*/
		/*Indicate no ROM present & pt to nxt array elm.*/
			rom_array[ra_index++] = 0 ;			
	}/*End of loop indentifying memory areas occupied by ROM	*/
#if (EQNX_VERSION_CODE > 131327)
	iounmap(caddr);
#endif /* Pat */

	rom_array_built = 0xFF ;/*Set falg indicating array built*/
	return ((unsigned char)0) ;
}  /* ROM_CHECK */

#endif	/* ISA_ENAB */

#define PCI_M1_MAX_BUS         256  /* Maximum number of busses */
#define BUS_NUM_SHIFT          16
#define PCI_M2_MAX_BUS         0x100
#define M1_SLOT_MAX            32  /* Max number of slots per bus */
#define M1_SLOT_ID_SHIFT       11
#define M2_SLOT_MAX            16
#define M2_SLOT_ID_SHIFT       8
#define PCI_M1_ENABLE_BIT      0x80000000
#define PCI_NULL_ID            0xFFFFFFFF
#define PCI_NULL_BYTE          0xFF
#define PCI_M1                 0x01
#define PCI_M2                 0x02
#define PCI_CONFIG_ADDRESS_REG 0xCF8
#define PCI_CONFIG_SPACE_ENABLE_REG 0xCF8
#define PCI_CONFIG_DATA_REG    0xCFC
#define PCI_BASE_ADDR          0xC000
#define PCI_FORWARD_REG        0xCFA
#define PCI_M2_ENABLE_BIT      0x80

int eqx_pci_buspresent(struct pci_csh *csh);
unsigned int eqx_bios32_service_directory(void);

struct bios32 {
   union {
      unsigned char c[16];
      struct {
         unsigned int signature;
         unsigned int entry;
         unsigned char revision;
         unsigned char length;
         unsigned char checksum;
         unsigned char res[5];
      } v;
   } q;
} ;

unsigned int eqx_bios32_service_directory(void)
{
   struct bios32 *b32;
   unsigned char check_sum;
   char *vaddr;
   int i, len;
   unsigned int b32_sig = ('_' << 24) + ('2' << 16) + ('3' << 8) + '_';

	vaddr = (char *)0xe0000;
   for(b32 = (struct bios32 *)vaddr; (unsigned long)b32 < 
		   (unsigned long)vaddr + 0x20000; b32++) {
      if (b32->q.v.signature != b32_sig)
         continue;
      len = b32->q.v.length * sizeof(struct bios32);
      for(i = 0, check_sum = 0; i < len; i++)
         check_sum += b32->q.c[i];
      if (check_sum)
         continue;
      return(b32->q.v.entry);
   }
return(0);
}

int eqx_pci_buspresent(struct pci_csh *csh)
{
	int            i, 
	               j,
	               xx,
                       bus_num, 
                       slot_id, 
	               index = 0;
	unsigned short  config_space_id;
	unsigned char  cse_save;

	if (!csh) {
		return(0);
	}

	/* Mechanism 1 */
	for(i = 0; i < PCI_M1_MAX_BUS; i++) {
		bus_num = i << BUS_NUM_SHIFT;
		for(j = 0; j < M1_SLOT_MAX; j++) {
			slot_id = j << M1_SLOT_ID_SHIFT;
			/*
			 * Write to the CONFIG_ADDRESS register
			 * so we can read the CONFIG_DATA register
			 */
	                outl( PCI_M1_ENABLE_BIT | bus_num | slot_id,PCI_CONFIG_ADDRESS_REG);
			config_space_id = inw(PCI_CONFIG_DATA_REG);
			if (config_space_id == EQUINOX_PCI_ID) {
				for(xx = 0; xx < sizeof(struct pci_csh00); 
				    xx+=sizeof(int)) {
					outl( PCI_M1_ENABLE_BIT | bus_num |
					     slot_id | xx,PCI_CONFIG_ADDRESS_REG);
					csh->cs.vl[xx/sizeof(int)] = 
						inl(PCI_CONFIG_DATA_REG);

					/* Fix for cost reduced 4/8 and 4P/LP */
					/* Boards - Revision > 8. Check Rev ID*/
					/* And if appropriate, set MSC bit.   */
					/* ID > 8  (bit 3 set) is UNIQUE      */
					/* to these boards		      */
					
					if (xx == 2 * sizeof(int))
						if ( csh->cs.vl[2] && 0x08) {
							csh->cs.vl[1] |= 0x02;
							outl( PCI_M1_ENABLE_BIT 
								| bus_num 
								| slot_id 
								| xx/2 ,
								PCI_CONFIG_ADDRESS_REG);
							outl( csh->cs.vl[1], 
							   PCI_CONFIG_DATA_REG);
						}
					/* End Fix for cost reduced PCI boards*/
				}
				csh->bus = i;
				csh->slot = j;
				csh++;
				index++;
				if (index >= maxbrd) {
					return(maxbrd);
}
			}
		}
	}

	if (index) {
		return(index);
	} 
	
	/* Mechanism 2 */
	cse_save = inb(PCI_CONFIG_SPACE_ENABLE_REG);
	outb(PCI_M2_ENABLE_BIT,PCI_CONFIG_SPACE_ENABLE_REG );
	if (inb(PCI_CONFIG_SPACE_ENABLE_REG) != PCI_M2_ENABLE_BIT) {
		outb(cse_save,PCI_CONFIG_SPACE_ENABLE_REG );
		return(0);
	}
	for(bus_num = 0; bus_num < PCI_M2_MAX_BUS; bus_num++) {
		outb(bus_num,PCI_FORWARD_REG );
		for(i = 0; i < M2_SLOT_MAX; i++) {
			slot_id = i << M2_SLOT_ID_SHIFT;
			config_space_id = inw(PCI_BASE_ADDR | slot_id | 00);
			if (config_space_id == EQUINOX_PCI_ID) {
				for(xx = 0; xx < sizeof(struct pci_csh); 
				    xx+=sizeof(short)) {
					csh->cs.vs[xx/sizeof(short)] = 
						inw(PCI_BASE_ADDR | slot_id |
						    xx);

					/* Fix for cost reduced 4/8 and 4P/LP */
					/* Boards - Revision > 8. Check Rev ID*/
					/* And if appropriate, set MSC bit.   */
					/* ID > 8  (bit 3 set) is UNIQUE      */
					/* to these boards		      */
					
					if (xx == 4 * sizeof(short))
						if ( csh->cs.vs[4] && 0x08) {
							csh->cs.vs[2] |= 0x02;
							outw( csh->cs.vs[2], 
							   PCI_BASE_ADDR 
							   | slot_id | xx/2);
							  
						}
					/* End Fix for cost reduced boards */
				}
				csh->bus = bus_num;
				csh->slot = i;
				csh++;
				index++;
				if (index >= maxbrd) {
					return(maxbrd);
				}
			}
		}
	} 
	outb(cse_save,PCI_CONFIG_SPACE_ENABLE_REG );
	return(index);
}

/* 
** rampadmn
*/

/********************************************************************************
 *                                                                              *
 *              REMOTE ACCESS MODEM POOL(RAMP) ADMIN/DIAG FACILITIES	        *
 *              ----------------------------------------------------        	*
 *                                                                              *
 * PURPOSE: To provide RAMP Adminsitrative/Diagnostic facilities and Diag/Admin	*
 *	    Poll Function as described in "Remote Access Modem Pool (RAMP) 	*
 *	    Software Services" documentation.					*
 *                                                                              *
 * NOTE: - Software was originally developed using:                             *
 *           o WATCOM 32 bit compiler                                           *
 *           o Phar Lap 386|linker                                              *
 *           o Debugged/tested in Phar Lap DOS Extender environment             *
 *	 - "MPA" is used throughout source and is the same as "RAMP"		*
 *	 - All page references refer to Plug and Play Specification Version 1.0	*
 *	   dated 5/28/93)							*
 *                                                                              *
 *                                                                              *
 * FILE:   RAMPADMN.C                                                           *
 * AUTHOR: BILL KRAMER                                                          *
 * REV:    06 (update "MPA_ADMIN_DIAG" in Header file, MPA.H, if change rev)    *
 * DATE:   10/02/97                                                             *
 *                                                                              *
 *                      E Q U I N O X   C O N F I D E N T I A L            	*
 *                      =======================================            	*
 *                                                                              *
 ********************************************************************************/
/*#page*/
/********************************************************************************
 *                                                                              *
 *                               REVISION HISTORY                               *
 *                               ================                               *
 *                                                                              *
 * REV	  DATE				                     COMMENTS           *
 * ===  ========        ======================================================= *
 *  06	09/30/97	Added conditional compilations to accommodate BSDI 	*
 *			build environment.  Following symbol, if defined, 	*
 *			indicates BSDI compilation:				*
 *				RAMP_BSDI  = BSDI compile			*
 *			NOTE: No changes were necessary for LINUX compilations	*
 *  05  09/09/96	Modified Plug and Play probe sequence to do 144 Reads	*
 *			required to isolate some newer PnP boards 		*
 *			(e.g. 33.6 Zoom)					*
 *  04  06/10/96	Added code to Admin/Poll function that saves slots' 	*
 *			Power State and save in	Slot Control Block.		*
 *  03	05/23/96	Modified function declarations to accomodate UNIX 	*
 *		        compiliers						*
 *  02	05/20/96	Modified PnP Probe functions so that a non-PnP board 	*
 *			is not Probed thus assuring when board registered it is	*
 *			recognized as a non-PnP board.  Also modified, PnP Probe*
 *			function 56 to recognize PnP Board Presence and		*
 *			therefore not Probe again.				*
 *  01	05/10/96	Initial release						*
 *                                                                              *
 ********************************************************************************/





/*#stitle HEADER FILE DECLARATIONS#											*/
/*#page*/
/*********************************************************
 * HEADER FILE DECLARATIONS                              *
 *********************************************************/

/*SSP Structure names (used to point to Global, Input, and Output registers)            */
/*NOTE: Macros below must be defined prior to inclusion of RAMP.H.  The macros below are*/
/*	necessary since various development environments utilized different structure	*/
/*	definitions for SSP Global, Input and output registers.  The definitions below	*/
/*	refer to structures defined in icp.h					*/


/*#stitle STRUCTURE DEFINITIONS/DEFINES/LITERALS DECLACARATIONS#*/
/*#page*/
/************************************************************************
 * Structure definitions & defines/literals                             *
 ************************************************************************/

#define	SSPCB_LIST_MAX	((unsigned int)32)	/*Max. number of SSPCBs	to srch	*/
						/* in SSPCB List - circular 	*/
						/*  (infinite) chain limit	*/

							



/************************
 *    MACROS/LITERALS	*
 ************************/











/*#stitle PUBLIC DECLACARATIONS#*/
/*#page*/
/************************************************************************************************
 * Public Declarations i.e. Items DEFINED in this compilation Referenced in other Compilations	 *
 ************************************************************************************************/


/****************
 *  Data Areas	*
 ****************/




/***********************
 *  Service Functions  *
 ***********************/

/*NOTE: See MPA.H	*/










/*#stitle EXTERNAL DECLACARATIONS#*/
/*#page*/
/********************************************************
 * DATA EXTERNAL to this compilation                    *
 ********************************************************/

/*Data areas defined in RAMPSRVC.C							*/
extern struct	admin_diag_hdr_struct	admin_diag_hdr ; /*Administrative & Diagnostic	*/
							 /*Services Header area		*/

/*#page*/


/*#stitle FORWARD REFERENCE DECLACARATIONS#*/
/*#page*/
/*********************************************************************************************************************
 * Forward Reference Declarations i.e. Functions that are INTERNAL to this compilation but are forward referenced    *
 *********************************************************************************************************************/

/*Plug and Play Probing Functions								*/
static void pnp_probe_start (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_02 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_03 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_04 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_05 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_06 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_07 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_08 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_09 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_10 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_11 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_12 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_13 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_14 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_15 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_16 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_17 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_18 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_19 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_20 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_21 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_22 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_23 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_24 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_24a (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			     ) ;

static void pnp_probe_fn_24b (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			     ) ;

static void pnp_probe_fn_24c (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			     ) ;

static void pnp_probe_fn_24d (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			     ) ;

static void pnp_probe_fn_25 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_26 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_27 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;
/*NOTE: Functions 28 & 29 deleted, see note at end of Function # 27		*/

static void pnp_probe_fn_30 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_31 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_32 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_33 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_34 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_35 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_36 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_37 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_38 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_39 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_40 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_41 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_42 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_43 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_44 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_45 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_46 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_47 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_48 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_49 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_50 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_51 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_52 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_53 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_54 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_55 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_56 (
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void pnp_probe_fn_57 (				/*R2*/
			     struct ssp_struct        *,
      			     struct slot_struct       *,
      			     volatile struct mpa_global_s      *,
			     volatile struct mpa_input_s       *,
      			     volatile struct mpa_output_s      *
			    ) ;

static void	pnp_clean_up_ssp (
				  struct slot_struct      *    slot,
			          unsigned char	               type
				 ) ;

/*Internal/Local Functions									*/
static unsigned char	pnp_frame_delay_chk (
					     unsigned short int,
		  			     volatile struct icp_gbl_struct      *,
					     struct slot_struct            *
					    ) ;











/*#stitle GLOBAL (STATIC) DATA AREA#*/
/*#page*/
/******************************************************************************************
 * Global Data Area i.e. Variables declared in Data Segment but local to this compilation *
 ******************************************************************************************/

/*PnP Initiation Key Sequence												*/
static unsigned char pnp_init_seq [] = {0x00, 0x00,
					0x6A, 0xB5, 0xDA, 0xED, 0xF6, 0xFB, 0x7D, 0xBE,
					0xDF, 0x6F, 0x37, 0x1B, 0x0D, 0x86, 0xC3, 0x61,
					0xB0, 0x58, 0x2C, 0x16, 0x8B, 0x45, 0xA2, 0xD1,
					0xE8, 0x74, 0x3A, 0x9D, 0xCE, 0xE7, 0x73, 0x39
				       }  ;







/*#stitle ADMIN/DIAG SERVICE - ENABLE/DISABLE PLUG & PLAY PROBE (mpa_srvc_pnp_probe_cntrl)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                        ENABLE/DISABLE PLUG & PLAY PROBE		       *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_pnp_probe_cntrl (struct ssp_struct      *  sspcb,
	                               	          	  unsigned char		    probe_ena)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*Variables used to access and search SSPCB List					*/
unsigned long int	svd_int_msk ;		/*Int Msk prior to Dsabl		*/
struct ssp_struct     	*sspcb_srch_ptr ;	/*Ptr to SSPCB in List			*/
unsigned int		silent_death_ctr ;	/*Silent death counter			*/

/*Variables used to access an SSPCB's MPA(s) and SLOTCBs				*/
unsigned int		mpa_index ;		/*MPA Array member			*/
unsigned int		num_slots ;		/*# Slots assoc'd w/MPA Array Member	*/
struct slot_struct     	*slot ;			/*MPA's/Eng's associated Slot Member	*/

/*General Working Variables								*/
unsigned int		i ;				/*Loop counter			*/




/************************
 *   START OF CODE	*
 ************************/

/*Validate caller's parameters								*/
if ( (sspcb            == (struct ssp_struct      *)0) || /*Does caller's SSP		*/
     (sspcb->in_use    != 0xFF)     ||			  /* Control Block		*/
     (sspcb->rev       != MPA_REV)  ||			  /*  appear sane?		*/
     (sspcb->signature != sspcb) )
	return (ERR_PNP_PROBE_SSPCB_BAD) ;		/*If no, exit			*/

  /*Set up to search for caller's SSPCB in Administrative & Diagnostic Service		*/
  /*Header's Chain by first gaining access to SSPCB list.  Gaining access to List	*/
  /*prevents this function from searching chain while chain being modified (via		*/
  /*Register SSP Service).  Also when List is acquired, Diag/Admin Poll function	*/
  /*and other Services are held off accessing SSPCB list until this function		*/
  /*completes										*/
while ( (mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk)) != 0xFF ) {} /*Spin till access granted*/

  /*Access to SSPCB list granted, SSPCB List is locked, Search for caller's SSPCB 	*/
for (i = SSPCB_LIST_MAX, sspcb_srch_ptr = admin_diag_hdr.sspcb_link; 
     (i != 0 )					     &&	 /*Srch SSPCB List for caller's	*/
     (sspcb_srch_ptr != (struct ssp_struct      *)0) &&	 /*SSPCB		 	*/
     (sspcb_srch_ptr != sspcb) ;			 /*NOTE: i = circular chain 	*/
     --i, sspcb_srch_ptr = sspcb_srch_ptr->sspcb_link ); /*	     safey check	*/
if (sspcb_srch_ptr != sspcb)				/*Caller's SSPCB in lst?	*/
{							/*If no, return error		*/
	mpa_fn_relse_sspcb_lst (svd_int_msk) ;		/*Release SSCP List		*/
	return (ERR_PNP_PROBE_SSPCB_BAD) ;		/*If no, exit			*/
}

  /*Caller's SSPCB found/valid and SSPCB List is Locked, set up & handle enable/disable	*/
  /*request										*/
if (probe_ena == 0xFF)					/*Enabling PnP Probing?		*/
{							/*If yes			*/
    /*Caller's Enabling SSPCB's PnP Probing, Go initialize each MPA's SLOTCB PnP Probe	*/
    /*state										*/
	if (sspcb->pnp_probe_ena != 0x00)		/*Probing already ENA?		*/
	{						/*If yes, return error		*/
		mpa_fn_relse_sspcb_lst (svd_int_msk) ;	/*Release SSCP List		*/
		return (ERR_PNP_PROBE_REDUND) ;
	}
    /*For caller's SSP assure each Modem Pool Slot's PnP area is initialized		*/
	for ( mpa_index = 0; mpa_index < ENGS_MPA; ++mpa_index )
	{						/*Loop through SSP's MPAs or Engs*/
		if (sspcb->mpa[mpa_index].base_chan == 0xFF ) /*Does MPA/ENG exist?	 */
			continue ;			      /*If no, chk nxt Ring postn*/
           /*MPA or Engine exists on Ring at mpa_index, assure PnP area initialized	 */
		num_slots = 				/*Get "current" Eng's # Slots	 */
		  ((sspcb->mpa[mpa_index].mpa_num_engs) == 1)  ? 16 :
		  (((sspcb->mpa[mpa_index].mpa_num_engs) == 2) ?  8 : 4 ) ;
		for (slot = sspcb->mpa[mpa_index].eng_head; /*Loop to init MPA's/Eng's	 */
		     num_slots != 0 ; 			    /* Slots PnP Probe Area	 */
		     --num_slots, ++slot )
		{
			slot->pnp_probe_state = 0x0000;	/*PnP Probe state = not init'd	 */
			slot->pnp_probe_funct = (void (*)(	/*PnP Probe Fn	 	 */
	      			     struct ssp_struct        *,
	      			     struct slot_struct       *,
	      			     volatile struct mpa_global_s      *,
				     volatile struct mpa_input_s       *,
	      			     volatile struct mpa_output_s      *))0 ;
			slot->pnp_clean_up   = (void (*)(struct slot_struct       *,
	      			     			 unsigned char              ))0 ;
		}
	}
   /*All MPA (or Engine) PnP areas that are associated with caller's SSP have	*/
   /*been initialized, set up so that Administrative & Diagnostic Poll function	*/
   /*is notified that SSP's PnP Probing has been enabled			*/
	if ( admin_diag_hdr.pnp_probe != (unsigned short int)0xFF ) /*1st time	*/
	{							   /* PnP enabld*/
		admin_diag_hdr.pnp_probe     = 0xFF ;	/*Indicate at least 1 	*/
							/*SSP PnP Probe enabled	*/
		admin_diag_hdr.pnp_sspcb     = sspcb ;	/*Set SSPCB Probe ptr	*/
		admin_diag_hdr.pnp_eng_index = 0x00 ; 	/*Pt Probe to 1st 	*/
							/* possible MPA or Eng	*/
	}
	sspcb->pnp_probe_ena     = 0xFF ;		/*Allow SSP PnP Probing	*/
							/* on caller's SSP	*/
}						/*End of PnP Probe enable	*/
else
{
     /*Caller Disabling SSP's PnP Probing, Go Clean Up any outstanding PnP	*/
     /*Probes									*/
	if (sspcb->pnp_probe_ena == 0x00)		/*Probing alrdy DSA?	*/
	{						/*If yes, return error	*/
		mpa_fn_relse_sspcb_lst (svd_int_msk) ;	/*Release SSCP List	*/
		return (ERR_PNP_PROBE_REDUND) ;
	}
	for ( mpa_index = 0; mpa_index < ENGS_MPA; ++mpa_index )
	{						/*Loop through SSP's MPAs or Engs*/
		if (sspcb->mpa[mpa_index].base_chan == 0xFF ) /*Does MPA/ENG exist?	 */
			continue ;			      /*If no, chk nxt Ring postn*/
           /*MPA or Engine exists on Ring at mpa_index, cancel any outstandng PnP Probing*/
		num_slots = 				/*Get "current" Eng's # Slots	 */
		  ((sspcb->mpa[mpa_index].mpa_num_engs) == 1) ? 16 :
		  (((sspcb->mpa[mpa_index].mpa_num_engs) == 2) ?  8 : 4 ) ;
		for (slot = sspcb->mpa[mpa_index].eng_head; /*Loop to cancel MPA's/Eng's */
		     num_slots != 0 ;			    /* Slot PnP Probing		 */
		     --num_slots, ++slot )
		{
			if ( (slot->slot_state      == 0x00)   && /*Is slot empty &	 */
/*R10*/			     (slot->pnp_probe_state != 0x0000) && /*PnP Probe = active?  */
     			     (slot->pnp_clean_up    != (void (*)(struct slot_struct      *, unsigned char))0) )
			{				   	/*If yes, cancel probe	 */
				(*slot->pnp_clean_up)(slot, (unsigned char)0xFF) ;
			}
			slot->pnp_probe_state = 0x0000 ;	/*Probe state=not init'd*/
		}
	}
	sspcb->pnp_probe_ena = 0x00 ;				/*Dsa PnP Probing on SSP */

     /*SSP's outstanding PnP Probes have been canceled, determine if any SSP still has 	 */
     /*PnP Probing enabled. If not, clear Administrative & Diagnostic Services Header's	 */
     /*PnP area such that Administrative & Diagnostic function is notified that SSP's 	 */
     /*PnP Probing is disabled							 	 */
	for ( silent_death_ctr = 0x200, sspcb_srch_ptr = admin_diag_hdr.sspcb_link ;
	     (sspcb_srch_ptr != (struct ssp_struct      *)0) &&	
	     (sspcb_srch_ptr->pnp_probe_ena == 0x00)	     &&
	     (silent_death_ctr != 0) 			     ;
     	     sspcb_srch_ptr = sspcb_srch_ptr->sspcb_link,
	     --silent_death_ctr				     ) ;
	if ( sspcb_srch_ptr == (struct ssp_struct      *)0 )	/*Any SSP have PnP enbled*/
	{							/*If no			 */
		admin_diag_hdr.pnp_probe     = 0x00 ;		/*Indicate no PnP Probing*/
		admin_diag_hdr.pnp_sspcb     = (struct ssp_struct      *)0 ;
		admin_diag_hdr.pnp_eng_index = 0x00 ;
	}
}						/*End of PnP Probe disable		 */

  /*Caller's SSP's PnP Probing Enable/Disable request complete, Unlock SSPCB List	 */
  /*and return to caller with good status						 */
mpa_fn_relse_sspcb_lst (svd_int_msk) ;		/*Unlock SSPCB List			 */
return (0x00) ;

}						/*End of EAN/DSA PnP Probing Fn	*/







/*#stitle ADMIN/DIAG SERVICE - ADMINISTRATIVE/DIAGNOSTIC POLL (mpa_srvc_diag_poll)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                        ADMINSTRATIVE/DIAGNOSTIC POLL			       *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_diag_poll (void) 

{

/************************
 *     LOCAL DATA	*
 ************************/

/*Plug and Play Probing variables							*/
unsigned long int	svd_int_msk ;		/*Int Msk prior to Dsabl		*/
struct ssp_struct     	*sspcb_srch_ptr ;	/*Ptr to SSP to Probe for PnP		*/
unsigned int		srch_index ;		/*Index to SSP's MPA/Eng. to Probe	*/
unsigned char		mpa_found ;		/*Flag indicatg if found an MPA to Probe*/
						/*  0x00 = Did not find MPA to probe	*/
						/*  0xFF = Found MPA/Eng to probe	*/
struct ssp_struct     	*probe_sspcb ;		/*Ptr to SSP to Probe for PnP		*/
unsigned int		probe_index ;		/*Index to SSP's MPA/Eng. to Probe	*/
unsigned int		num_slots_probe ;	/*Number of Slots to Probe		*/
struct slot_struct     	*slot ;			/*Pointer to Slot to Probe		*/

/*SSP Channel/Slot Variables								*/
volatile struct mpa_global_s      *  gregs ;	/*SSP's Global registers		*/
volatile struct mpa_input_s       *  iregs ;	/*Slot's Input Registers		*/
volatile struct mpa_output_s      *  oregs ;	/*Slot's Output Regsiters		*/
volatile struct cin_bnk_struct      *actv_bank; /*Slot's active bank pointer		*/ /*R2*/

/*Variables use to MAP/UNMAP SSP Registers into memory					*/
void     		*unmap_parm=0 ;		/*Parameter used by UNMAP function	*/

/*Variables used in accessing SSP registers						*/
unsigned short int	ssp_ushort_var ;	/*Used to modify 16 bit	SSP registers	*/

/*General Working Variables								*/
unsigned int		infinite_chk ;		/*Loop counter to guard against		*/
						/*infinite/circular SSPCB chain		*/




/************************
 *   START OF CODE	*
 ************************/

/*Assure that System Functions have been defined						*/
if ( admin_diag_hdr.init_flag != 0xFFFF )	/*Have Fn addrs been spec'd?			*/
	return (ERR_DIAG_POLL_NO_FNS) ;		/*If no, error					*/

/*Set up and determine if need to (or can) perform Plug and Play Probing			*/
if ( admin_diag_hdr.pnp_probe != 0xFF )		/*Is PnP Probing enabled on at least one SSP	*/
	goto  admin_diag_pnp_poll_cmplt ;	/*If no, continue other Poll activities		*/
if ( mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk) == 0x00 ) /*Did gain access to SSPCB list? 		*/
	goto  admin_diag_pnp_poll_cmplt ;	      /*If no, cont Poll activities  		*/

/*Some SSP's PnP Probe is enabled and have gained access (and locked) SSPCB List search for	*/
/*SSPCB and MPA to Probe									*/
infinite_chk    = SSPCB_LIST_MAX ;		/*Init infinite chain safety counter		*/
mpa_found 	= 0x00 ;			/*Set MPA found flag = not found		*/
sspcb_srch_ptr  = admin_diag_hdr.pnp_sspcb ;	/*Get ptr to SSPCB to search			*/
srch_index      = admin_diag_hdr.pnp_eng_index;	/*Get SSPCB's MPA array index			*/
do
{
	probe_sspcb = sspcb_srch_ptr ;		/*Anticapte finding SSPCB and MPA/ENG to Probe	*/
	probe_index = srch_index ;
	if ( ++srch_index >= ENGS_MPA )		/*Pt to nxt SSP/MPA to Probe			*/
	{
		srch_index = 0 ;
		sspcb_srch_ptr = (sspcb_srch_ptr->sspcb_link != (struct ssp_struct      *)0) ?
			          sspcb_srch_ptr->sspcb_link : admin_diag_hdr.sspcb_link ;
	}
	if ( (probe_sspcb->pnp_probe_ena == 0xFF) 	       && /*Is PnP Probing enabled	*/
	     (probe_sspcb->mpa[probe_index].base_chan != 0xFF)  ) /* and does MPA exist?	*/
	{							  /*If found MPA to probe	*/
		mpa_found = 0xFF ;				  /*Indicate found MPA		*/
		break ;
	}
} while ( ( (sspcb_srch_ptr != admin_diag_hdr.pnp_sspcb)   ||     /*End of srch for MPA to Probe*/
	    (srch_index     != admin_diag_hdr.pnp_eng_index) ) &&
	  (--infinite_chk != 0x00)			   ) ;


  /*Search for SSP and MPA/Eng to Probe complete, determine if found an MPA/Eng to search	*/
  /*NOTE: mpa_found      = Flag indicating if able to identify SSP/MPA to Probe for Plug & Play	*/
  /*				0x00 = NO SSP/MPA found						*/
  /*				0xFF = identified SSP/MPA to Probe for Plug and Play boards	*/
  /*	sspcb_srch_ptr = Pointer to next SSP to probe when Poll function called again		*/
  /*	srch_index     = Index (in SSP's MPA array) to nxt MPA to Probe when Poll called again	*/
  /*	probe_sspcb    = Pointer to SSP to Probe for PnP					*/
  /*	probe_index    = Index (in SSP's MPA array) to MPA to Probe for PnP boards		*/
if ( mpa_found != 0xFF )				/*Did find an MPA/Eng to Probe?		*/
	goto  admin_diag_pnp_no_probe ;			/*If no, continue Admin/Diag Poll  	*/
admin_diag_hdr.pnp_sspcb     = sspcb_srch_ptr ;		/*Sv SSPCB search ptr for nxt Poll	*/
admin_diag_hdr.pnp_eng_index = srch_index ;		/*Sv SSPCB's MPA array index	  	*/


  /*Found MPA/Engine to perform Plug and Play Probing on, if appropriate MAP SSP Registers	*/
  /*NOTE: probe_sspcb = Pointer to SSP to Probe for PnP						*/
  /*	  probe_index = Index (in SSP's MPA array) to MPA to Probe for PnP boards		*/
if ( probe_sspcb->map_fn != (void      *(*)(void      *))0 ) /*Are SSP registers paged?		*/
{							/*If SSP register page, go map in regs	*/
	unmap_parm = (*probe_sspcb->map_fn)(probe_sspcb->map_parm) ; /*Go MAP in SSP registers	*/
}

  /*If required, SSP MAPPed into memory, loop through all slots performing next "state machine"	*/
  /*function											*/
gregs 		= probe_sspcb->global ;			/*Point to SSP's Global regsiters	*/
num_slots_probe = ((probe_sspcb->mpa[probe_index].mpa_num_engs) == 1) ? 16 :
		 (((probe_sspcb->mpa[probe_index].mpa_num_engs) == 2) ?  8 : 4 ) ;

for (slot = probe_sspcb->mpa[probe_index].eng_head;	/*Loop thru all slot's PnP Probe Functs	*/
     num_slots_probe != 0 ;
     --num_slots_probe, ++slot )
{
	iregs     = slot->input ;			/*Set ptr to channel's Input registers	*/
/*R2*/	actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Is Bank A active?	*/
		      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  	*/
		      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  	*/
	oregs 	  = slot->output ;			/*Set ptr to channel's Output registers	*/
/*R4*/	slot->power_state = ( ((actv_bank->bank_sigs) & 0x0800) == 0x0000 )  ?
		(unsigned short int)0x0000 : (unsigned short int)0x00FF 	    ;
	if ( slot->slot_state != 0x00 )			/*Is Slot empty?			*/
		continue ;				/*If no, chk PnP Probing on next slot	*/
	if ( slot->pnp_probe_state == 0x0000 )		/*Has Slot been init'd for PnP Probing?	*/
	{						/*If no, initialize PnP state Function	*/

/*R2*/	   /*First time Slot is being Probed for Plug and Play Board - chk if board exists and	*/
	   /*if so assume it is a non-PnP board that has yet to be registered.  Therefore do not*/
	   /*initiate PnP Probe.								*/
/*R2*/		if ( (actv_bank->bank_sigs & 0x0001) == 0 ) /*Is Brd present?		*/
			continue ;				   /*If yes, assume unreg'd	*/
								   /*unreg'd non-PNP board	*/
	   /*No board present in Slot - save state of SSP channel registers and initialize 	*/
	   /*registers so can start PnP Probe and isuue I/O to board etc.			*/
		slot->pnp_sv_cout_flow_config = oregs->cout_flow_cfg ;
		slot->pnp_sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
		slot->pnp_sv_cout_xon_1       = oregs->cout_xon_1 ;
		slot->pnp_clean_up            = (void (*)(struct slot_struct      *, unsigned char))&pnp_clean_up_ssp ;
		  /*	*** Transmint & Receive char attributes (8,N,1)				*/
		ssp_ushort_var  = (iregs->cin_char_ctrl & 0xFFE0) ;	/*Set up SSP Input to 	*/
		ssp_ushort_var |= 0x03 ;				/*recv 8 bits, no parity*/
		iregs->cin_char_ctrl = ssp_ushort_var ;
		oregs->cout_char_fmt = 0x03 ;				/*SSP output = 8, none	*/
		  /*	*** Transmint & Receive Baud Rate = Top rate				*/
		iregs->cin_baud = 0x7FFF ;				/*Input baud rate = TOP	*/
		oregs->cout_baud = 0x7FFF ;			/*Output baud rate = TOP*/
		while ( 1 == 1 )					/*Loop to reset SSP 	*/
		{							/* internal baud ctrs	*/
			while ( gregs->gicp_chnl == slot->chan_num ) {}	/*  assures baud setting*/
			iregs->cin_baud_ctr &= 0xC000 ;			/*   takes affect imedly*/
			oregs->cout_int_baud_ctr = 0 ;
			while ( gregs->gicp_chnl == slot->chan_num ) {}	
			if ( ((iregs->cin_baud_ctr & 0x3FFF) == 0) &&
		              (oregs->cout_int_baud_ctr    == 0) )
					break ;
		}
		  /*	*** Set Xtra DMA (since running at TOP baud rate) & Send XON Char	*/
		oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;
		  /*	*** Flow Control, MPA-to-SSP = "MUX not Connected"			*/
		  /*	                  SSP-to-MPA = Output Cntrl sig 3, 			*/
		iregs->cin_susp_output_lmx |= 0x20 ;			/*MUX not Connected	*/
		iregs->cin_q_ctrl |= 0x20 ;				/*Set Input Flow Control*/
		  /*	*** Clear internal/external Loop backs					*/
		oregs->cout_lmx_cmd &= 0xFC ;			/*Clear loop backs	*/
		  /*    *** Set up to call 1st PnP Probe function				*/
		slot->pnp_probe_funct = pnp_probe_start ;
		slot->pnp_probe_state = 0x0080 ;		/*Indicate PnP Probing initiated*/
	}

   /*Continue Slot's PnP Probe by calling PnP Probe function					*/
	(*slot->pnp_probe_funct)(				/*Call Slot's PnP Probe Function*/
				 probe_sspcb,			/*  - Probe Slot's SSPCB	*/
				 slot,				/*  - Probe Slot's SLOTCB	*/
				 gregs,				/*  - Probe Slot's Global regs	*/
				 iregs,				/*  - Probe Slot's Input regs	*/
				 oregs				/*  - Probe Slot's Output regs	*/
				) ;
}								/*End of for loop Probing MPA	*/
								/* Slot's for PnP boards	*/

/*Plug and Play Probing complete, UNMAP SSP if neccessary					*/
if ( probe_sspcb->map_fn != (void      *(*)(void      *))0 ) /*Are SSP registers paged?		*/
{							/*If SSP register page, go map in regs	*/
	(*probe_sspcb->unmap_fn)(unmap_parm) ; 		/*Go UNMAP in SSP registers		*/
}


/*Unlock sspcb list and perform next Administrative/Diagnostic Poll function			*/
admin_diag_pnp_no_probe:
mpa_fn_relse_sspcb_lst (svd_int_msk) ;			/*Unlock SSPCB List			*/

/*Plug and Play Probing complete, perform next Admin/Diag activity				*/
admin_diag_pnp_poll_cmplt:				/*Pt when PnP Polling complete		*/

/*No more Administrative/Diagnostic activities to perform, return to caller with good status	*/
return (0x00) ;

}						/*End of mpa_srvc_diag_poll Funct*/







/*#stitle ADMIN/DIAG SERVICE - PnP PROBE FUNCTIONS#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *              INITIAL PnP PROBE FUNCTION - ISSUE HARD RESET TO SLOT	       *
 *                                                                             *
 *******************************************************************************/

static void pnp_probe_start (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
     			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Set slot's/channel's control signals to indicate to MPA to Hard Reset Modem Board (if exists)				*/
/*Note: Hard Reset causes any fully configured PnP board to reset and enter "Wait for Key" state thus MPA no longer	*/
/*	recognizes board.  Also, Hard Reset clears AUX Reg's PnP bit and Read Toggle bit.  As result of PnP bit	being	*/
/*	cleared Modem Pool will recognize a non-Plug and Play board.  Hence Hard Reset provides a "window" in Plug and	*/
/*	Play probe cycle where non-plug and play boards can be recognized.						*/
oregs->cout_ctrl_sigs = 0x1CC ;				/*Assert ISA Reset to Reset Modem Board (if exists)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;		/*Snap shot for timing						*/
slot->read_toggle     = 0x00 ;				/*UART I/O Read toggle						*/

/*Exit function & delay 8 frames thus allowing time for MPA to "see" Modem Reset					*/
slot->pnp_probe_funct = pnp_probe_fn_02 ;
return ;

}	/*END of PnP Hard Reset Modem - 1st function, pnp_probe_start							*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *      2nd PnP PROBE FN - HARD RESET COMPLETE, INITIATE WAIT FOR KEY	       *
 *                                                                             *
 *******************************************************************************/

static void pnp_probe_fn_02 (struct ssp_struct *sspcb,		
      			     struct slot_struct  *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;
oregs	   = oregs ;

/*Delay 8 frames - allow time for MPA to "see" Hard Reset								*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 ) /*Has 8 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Assure delay of at least 2 millisecond to allow time for PnP board to initialize (see Plug and Play spec. Ver 1.0,	*/
/*5/28/93, p. 19).  If PnP Board present it is now in "Wait for Key" State						*/
oregs->cout_ctrl_sigs   = (slot->pnp_sv_cout_cntrl_sig & 0x0237) | 0x0004 ; /*Restore control signals			*/
slot->pnp_frame_ctr     = gregs->gicp_frame_ctr ;			    /*Snap shot for timing			*/
slot->pnp_probe_funct   = pnp_probe_fn_03 ;
return ;

}	/*END of PnP - 2nd function, pnp_probe_fn_02									*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *      	 3rd PnP PROBE FN - Plug and PLAY BOARD RESET DELAY	       *
 *                                                                             *
 *******************************************************************************/

static void pnp_probe_fn_03 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 16 frames thus assuring at least 2 millisecond delay (allow time for PnP board to init after Hard Reset		*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x10, gregs, slot ) == 0 ) /*Has 10 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

  /*Have delay long enough for PnP board to Reset. Note: If PnP board present, Hard Reset places PnP board in "Wait for	*/
  /*Key" state (see Plug and Play spec. Version 1.0, 5/28/93, p. 15).  In order to place PnP Board into "Sleep" State,	*/
  /*need to send Initiation Key, 34 OUTs to PnP "Address Port" @ I/O addr 0x279 (see PnP spec. pages 13, 18, and 53)	*/
  /*The steps required by the Modem Pool to do this are as follows (note: each step is a seperate function)		*/
  /*    1. In order to address MPA Slot "AUX register", Set chan's control signals to 0x199				*/
  /*    2. Set PnP I/O bit in Aux register by xmit'g 0x20								*/
  /*    3. In order to address PnP "Address Port" (i.e. I/O addr 0x279), Set chan's control signals to 0x111.		*/
  /*    4. Sending Initiation Key (34 Output bytes to PnP Address Port)							*/
oregs->cout_ctrl_sigs = 0x199 ;						/*Addr MPA's AUX reg				*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_04 ;
return ;

}	/*End pnp_probe_fn_03												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *      	 4th PnP PROBE FN - Wait till AUX Reg addressed		       	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_04 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see AUX Reg being addressed							*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to MPA's AUX Reg,	*/
/*write to AUX register setting PnP I/O bit										*/
oregs->cout_xon_1 	  = 0x20 ;					/*Set AUX reg PnP I/O bit			*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr	  = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_05 ;
return ;

}	/*End pnp_probe_fn_04												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *      	 5th PnP PROBE FN - Wait till AUX Reg Receives PnP bit		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_05 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for AUX Reg byte (with PnP bit set) to be sent									*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Is AUX reg char xmit done?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*MPA's AUX Reg PnP bit set, start sending Initiation Key by first addressing PnP's Address Port (0x279)		*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_06 ;
return ;

}	/*End pnp_probe_fn_05												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           6th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_06 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, Start sending PnP Initiation Key (see Plug and Play spec. Version 1.0, 5/28/93, p. 18 and 53)			*/
slot->pnp_probe_parm      = 0x00 ;					/*Initiate parm to strt of PnP Init. Key sequnce*/
oregs->cout_xon_1 	  = pnp_init_seq [slot->pnp_probe_parm++] ;	/*Set AUX reg PnP I/O bit			*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_07 ;
return ;

}	/*End pnp_probe_fn_06												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *   7th PnP PROBE FN - Send PnP Initiation Key Sequence to PnP Address Port	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_07 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for a byte of PnP Initiation Key to be sent (then send next byte)						*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Initiation Key byte been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Initiation Key Sequence byte has been sent, check if sent entire byte sequence sent and if not send next byte	*/
if ( slot->pnp_probe_parm < sizeof(pnp_init_seq) )			/*Have completed PnP Initiation Sequence?	*/
{									/*If no, send next byte in sequence		*/
	oregs->cout_xon_1 = pnp_init_seq [slot->pnp_probe_parm++] ;		/*Set AUX reg PnP I/O bit		*/
	oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char			*/
	slot->pnp_frame_ctr = gregs->gicp_frame_ctr ;				/*Snap shot for timing			*/
	return ;
}

/*PnP Initiation Sequence sent, If PnP Board present it is now in "Sleep" State, (see Plug and Play spec. Ver 1.0, 	*/
/*5/28/93, p. 18).  Now, clear AUX Register's PnP bit thus providing Modem Pool time to see if a non-PnP Modem Board	*/
/* has been inserted into slot												*/
oregs->cout_ctrl_sigs = 0x199 ;						/*Addr MPA's AUX reg				*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_08 ;
return ;

}	/*End pnp_probe_fn_07												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *    8th PnP PROBE FN - Wait till AUX Reg addressed (clear PnP Port access)	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_08 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see AUX Reg being addressed							*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to MPA's AUX Reg,	*/
/*write to AUX register clearing PnP I/O bit										*/
oregs->cout_xon_1	  = 0x00 ;					/*Clear AUX reg PnP I/O bit			*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct 	  = pnp_probe_fn_09 ;
return ;

}	/*End pnp_probe_fn_08												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *     		9th PnP PROBE FN - Wait till AUX Reg Receives PnP bit clear	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_09 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for AUX Reg byte (with PnP bit cleared) to be sent								*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Is AUX reg char xmit done?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*MPA's AUX Reg PnP bit cleared, delay thus allowing Modem Pool time to recognize if a non-PnP Modem is present		*/
oregs->cout_ctrl_sigs = (slot->pnp_sv_cout_cntrl_sig & 0x0233) ; /*R5*/	/*Set Control Signals back to default		*/
									/*Note: MPA Input Flow Control bit off thus	*/
									/*	stopping MPA from sending any input 	*/
									/*	chars.  This is done here since this is	*/
									/*	last time UART control signals are 	*/
									/*	changed prior to readg Serial Identifier*/
									/*	required to isolate board.  See fns	*/
									/*	starting at pnp_probe_fn_24		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot so can delay			*/
slot->pnp_probe_funct = pnp_probe_fn_10 ;
return ;

}	/*End pnp_probe_fn_09												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  10th PnP PROBE FN - Delay to allow MPA time to recognize a non-PnP Modem	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_10 (struct ssp_struct *sspcb,	
      			     struct slot_struct *slot,
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 20 frames - allow MPA time to recognize a non-PnP Modem								*/
if ( pnp_frame_delay_chk ( (unsigned short int)20, gregs, slot ) == 0 )   /*Has 20 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have given enough of a window for Modem Pool to recognize a non-PnP Modem, set up so can address PnP ports (again) by	*/
/*setting Modem Pool AUX register's PnP bit										*/
oregs->cout_ctrl_sigs = 0x199 ;						/*Addr MPA's AUX reg				*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_11 ;
return ;

}	/*End pnp_probe_fn_10												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *      	 11th PnP PROBE FN - Wait till AUX Reg addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_11 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see AUX Reg being addressed							*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to MPA's AUX Reg,	*/
/*write to AUX register setting PnP I/O bit										*/
oregs->cout_xon_1 = 0x20 ;						/*Set AUX reg PnP I/O bit			*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_12 ;
return ;

}	/*End pnp_probe_fn_11												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *      	 12th PnP PROBE FN - Wait till AUX Reg Receives PnP bit		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_12 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for AUX Reg byte (with PnP bit set) to be sent									*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Is AUX reg char xmit done?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*MPA's AUX Reg PnP bit set and if PnP Board present it is in "Sleep" State, Reset PnP Card Select Number (CSN) by	*/
/*writing 0x04 to PnP Configuration Control reg (PnP reg @ 0x02). See Plug and Play spec. Version 1.0, 5/28/93, p. 45 )	*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_13 ;
return ;

}	/*End pnp_probe_fn_12												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           13th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_13 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x02 (Config Control) by writing 0x02 to PnP Address Port			*/
oregs->cout_xon_1 	  = 0x02 ;					/*Send PnP Control Register # to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_14 ;
return ;

}	/*End pnp_probe_fn_13												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  14th PnP PROBE FN - Set up to send Reset CSN to PnP Control Register 0x02	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_14 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Config Control register # been sent to PnP Address Port							*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Config Control reg # been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Control Register 0x02, now set control signals to address PnP's Write Data Port*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_15 ;
return ;

}	/*End pnp_probe_fn_14												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           15th PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_15 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, by writing a 0x04 to Write Data Port - PnP's Configuration Control register bit 2 is set thus causing PnP	*/
/*Board to zero its Card Select Number (CSN)										*/
oregs->cout_xon_1 	  = 0x04 ;					/*Send PnP Reset CSN bit/command		*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_16 ;
return ;

}	/*End pnp_probe_fn_15												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 16th PnP PROBE FN - Set up to Send Wake command via PnP Control Register 0x03*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_16 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for Reset CSN bit/command to be sent (i.e. 0x04 sent to PnP Control Register 0x02)				*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Reset CSN Command bit been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

  /*PnP Board's CSN now reset (CSN = 0) and Board still in "Sleep" sate, set up to place board in "Isolation" state by	*/
  /*writing a zero (0) to PnP Control register 0x03 (Wake register).  See Plug and Play spec. Version 1.0, 5/28/93,	*/
  /*pages 13, 15 & 46).  The steps required by the Modem Pool to do this are as follows (note: each step is a seperate	*/
  /* function):														*/
  /*    1. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*    2. Output 0x03, writes 0x03 to PnP Address Port (indicates to PnP board that write_data addresses Wake register)*/
  /*    3. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*    4. Output 0x00, writes 0x00 to PnP Wake register (causes PnP board to enter isolation state)			*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_17 ;
return ;

}	/*End pnp_probe_fn_16												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           17th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_17 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,	
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x03 (Wake) by writing 0x03 to PnP Address Port				*/
oregs->cout_xon_1 	  = 0x03 ;					/*Send PnP Control Register # to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_18 ;
return ;

}	/*End pnp_probe_fn_17												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  		18th PnP PROBE FN - Set up to address PnP Write Data Port 	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_18 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Config Control register # been sent to PnP Address Port							*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Config Control reg # been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Control Register 0x03 (WAKE), now set control signals to address PnP's Write	*/
/*Data Port														*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_19 ;
return ;

}	/*End pnp_probe_fn_18												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           19th PnP PROBE FN - Wait till PnP Write Data Port addressed and	*
 *				 then send 0 (Wake) to PnP Control Reg 0x03	*
 *				 thus causing PnP board to enter Isolation State*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_19 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, by writing a 0x00 to Write Data Port - PnP's Wake register is cleared thus causing PnP Board to enter	*/
/*Isolation state (see PnP spec. pages 13, 18, and 46)									*/
oregs->cout_xon_1 	  = 0x00 ;					/*Send PnP Reset CSN bit/command		*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_20 ;
return ;

}	/*End pnp_probe_fn_19												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *    20th PnP PROBE FN - Wait till data sent to PnP Wake register & Board in 	*
 *		          Isolation state					*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_20 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for data to be sent to Wake register (i.e. 0x00 sent to PnP Control Register 0x03)				*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Reset CSN Command bit been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

  /*PnP Board's now in Isolation State, set board's Read Data Port to 0x27B (see PnP spec. p. 15 & 45). The steps	*/
  /*required by the Modem Pool to do this are as follows (note: each step is a seperate function)			*/
  /*    1. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*    2. Output 0x00, wrt 0x00 to PnP Address Port (indicates to PnP board that write_data addresses Set RD Data Port)*/
  /*    3. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*    4. Output 0x9E, sets PnP Read Data Port to 0x27B (0x9E = 0x27b >> 2)						*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_21 ;
return ;

}	/*End pnp_probe_fn_20												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           21st PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_21 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x00 (Read Data Port) by writing 0x00 to PnP Address Port			*/
oregs->cout_xon_1 	  = 0x00 ;					/*Send PnP Control Register # to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_22 ;
return ;

}	/*End pnp_probe_fn_21												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  22nd PnP PROBE FN - Set up to send PnP Read Data Port address to PnP 	*
 *			Control Register 0x00					*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_22 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Config Control register # been sent to PnP Address Port							*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Config Control reg # been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Control Register 0x00, now set control signals to address PnP's Write Data Port*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_23 ;
return ;

}	/*End pnp_probe_fn_22												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           23rd PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_23 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, write a 0x9E to Write Data Port - thus causing PnP to set its Read Data Port to 0x27B (see spec. page 7,	*/
/*15, and 45)														*/
oregs->cout_xon_1 	  = (0x27B >> 2) ;				/*Send PnP Read Data Port address		*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_24 ;
return ;

}	/*End pnp_probe_fn_23												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 24th PnP PROBE FN - Set up to place PnP board into Configuration state	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_24 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for PnP Read Data Port Address to be sent (i.e. 0x9E sent to PnP Control Register 0x00)				*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Reset CSN Command bit been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*R5*/
/*PnP Board's Read Data Port address is now set to 0x27B, Read Serial Identifier from Data Port - requires 144 - 8 bit	*/
/*reads of Data Port to isolate card as per PnP spec., see pages 9, 19 & 25 (Zoom 33.6 SVD Faxmodem, Model 2802 	*/
/*requires this).													*/
/*NOTE: Since Reading PnP Read Data port causes a byte to be input'd and when a byte is input'd SSP determines line is	*/
/*	  no longer in "Break" state thusly clearing the Break status bit.  This, in turn, would cause a Driver to	*/
/*	  falsely determine that a Modem board is present.  Therefore, MPA was flowed off in function pnp_probe_fn_09	*/
/*The steps required by the Modem Pool to read the Serial identifier are as follows (note: each step is a seperate 	*/
/*function):														*/
/*    1. Set chan's control signals to 0x111 (indicates to MPA to address PnP Address Port, 0x279)	 		*/
/*    2. Xmit 0x01 to PnP Address port (indicates to PnP board that a read operation addresses Serial Isolation 	*/
/*	 register, see p. 25 & 45)											*/
/*    3. In order to address "AUX register", Set chan's control signals to 0x199					*/
/*    4. In order to read PnP I/O port 0x27B, Xmit 0x23 to AUX register (toggling bit 3)				*/
/*R5*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Set Cntrl Sigs to address PnP port 0x279	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_24a ;
return ;

}	/*End pnp_probe_fn_24												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 24a PnP PROBE FN - Wait for MPA to see Output Control Signals indicating to 	*
 *		      to address (direct output to) PnP Address Port (@0x279)	*
 *                                                                             	*
 ********************************************************************************/

/*R5*/
static void pnp_probe_fn_24a (struct ssp_struct *sspcb,
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see Control Signals								*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Serial Isolation Register by writing 0x01 to PnP Address Port					*/
oregs->cout_xon_1 	  = 0x01 ;					/*Send PnP Control Register # to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_24b ;
return ;

}	/*End pnp_probe_fn_24a												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  22nd PnP PROBE FN - Set up to send PnP Read Data Port address to PnP 	*
 *			Control Register 0x00					*
 *                                                                             	*
 ********************************************************************************/

/*R5*/
static void pnp_probe_fn_24b (struct ssp_struct *sspcb,	
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Config Control register # been sent to PnP Address Port							*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Config Control reg # been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Control Register 0x01 (Serial Isolation register), set Control Signals to	*/
/*address AUX register (so can initiate reads of PnP port 0x27B and thus receive Serial Identifier bits.		*/
oregs->cout_ctrl_sigs = 0x199 ;						/*Addr MPA's AUX reg				*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_24c ;
return ;

}	/*End pnp_probe_fn_24b												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *      	 24c PnP PROBE FN - Wait till AUX Reg addressed			*
 *                                                                             	*
 ********************************************************************************/

/*R5*/
static void pnp_probe_fn_24c (struct ssp_struct *sspcb,
      			     struct slot_struct *slot,	
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see AUX Reg being addressed							*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to MPA's AUX Reg,	*/
/*write to AUX register toggling "read toggle" thus causing MPA to read a byte from PnP Data Port (gets a bit of board's*/
/*Serial Identifier).													*/
slot->pnp_probe_parm    = 0x00 ;					/*Initiate parm to count Serial Identifier Reads*/
slot->read_toggle       ^= 0x08 ;					/*Save I/O Read toggle state			*/
oregs->cout_xon_1       = 0x23 | slot->read_toggle ;			/*Set up & send AUX Reg value =			*/
oregs->cout_flow_cfg ^= 0x10 ;					/* Read PnP I/O Port 0x27B			*/
slot->pnp_frame_ctr     = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct   = pnp_probe_fn_24d ;
return ;

}	/*End pnp_probe_fn_24c												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *   24d PnP PROBE FN - Continue/complete Read of Serial Identifier bits from	*
 *			PnP Data Port (0x27B)					*
 *                                                                             	*
 ********************************************************************************/

/*R5*/
static void pnp_probe_fn_24d (struct ssp_struct *sspcb,	
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for write of AUX Reg byte to complete.  Note: Byte value written indicates to MPA to Read a byte from PnP Data 	*/
/*Port (0x27B).  Note: Byte received is part of board's (if it exists) Serial Identifier and actually represents only	*/
/*1 bit of Identifier													*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Aux Byte been sent?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*Read a byte of board's (if it exists) Serial Identifier, check if done reading a bytes (bits)				*/
if ( ++slot->pnp_probe_parm < 72*2 )					/*Have completed reading Serial Identifier bits?*/
{									/*If no, read next byte				*/
	slot->read_toggle       ^= 0x08 ;				/*Save I/O Read toggle state			*/
	oregs->cout_xon_1       = 0x23 | slot->read_toggle ;		/*Set up & send AUX Reg value =			*/
	oregs->cout_flow_cfg ^= 0x10 ;				/* Read PnP I/O Port 0x27B			*/
	slot->pnp_frame_ctr     = gregs->gicp_frame_ctr ;		/*Snap shot for timing				*/
	return ;
}

/*Completed reading PnP board's Serial Identifier (assuming board exists), board (if exists) now isolated, set up to	*/
/*place board into Configuration state by writing 1 to PnP Card Number Select register (reg 0x06).  See Plug and Play	*/
/*spec. pgs 13, 18 & 46).  The steps required by the Modem Pool to do this are as follows (note: each step is a 	*/
/*seperate function):													*/
/*    1. Set chan's control signals to 0x111 (address PnP Address Port)	 						*/
/*    2. Output 0x06, writes 0x06 to PnP Address Port (indicates to PnP board that write_data addresses Card Select	*/
/*	 Number register)												*/
/*    3. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
/*    4. Output 0x01, writes 0x01 to PnP Card Select Number (causes PnP board to enter Configuration state)		*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_25 ;
return ;

}	/*End pnp_probe_fn_24c												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           25th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_25 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x06 (Read Data Port) by writing 0x06 to PnP Address Port			*/
oregs->cout_xon_1 	  = 0x06 ;					/*Send PnP Control Register # to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_26 ;
return ;

}	/*End pnp_probe_fn_25												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  26th PnP PROBE FN - Set up to send PnP Card Select Number Port address to 	*
 *			PnP Control Register 0x06				*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_26 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Config Control register # been sent to PnP Address Port							*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Config Control reg # been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Card Select Number Register 0x06, now set control signals to address PnP's	*/
/*Write Data Port													*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_27 ;
return ;

}	/*End pnp_probe_fn_26												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           27th PnP PROBE FN - Wait till PnP Write Data Port addressed and	*
 *				 write board's CSN				*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_27 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, write a 0x01 to Write Data Port - thus causing PnP to set its Card Select Number and enter Configuration	*/
/*state (see spec. page 7, 16, 18, and 46) from Isolation state								*/
oregs->cout_xon_1 	  = 0x01 ;					/*Send PnP CSN Number				*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_30 ;
return ;

}	/*End pnp_probe_fn_27												*/
/*NOTE: Functions 28 & 29 were deleted.  It was originally intended that these functions would read attempt to read CSN	*/
/*	and if CSN not read then PnP Probe would start all over.  If CSN successfully read then Probing would continue	*/
/*	at function 30.  Functions 28 & 29 can not be done since reading CSN (if PnP board present) causes a byte to be	*/
/*	input'd.  When a byte input'd SSP determines line no longer in "Break" state and hence clears Break status bit.	*/
/*	This, in turn, causes Driver to falsely determine the presence of a board.					*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *      	 30th PnP PROBE FN - Wait till PnP CSN sent and start to assign	*
 *				     I/O address				*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_30 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for PnP CSN to be sent												*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Is CSN xmit done?				*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

  /*CSN number assigned (if board exists) and board is in configuration state.  Set up and assign to board a base I/O	*/
  /*address of 0x3F8 (COM1) and an Interrupt (IRQ4 - COM1).  Once assigned activate PnP board.  The steps required by	*/
  /*Modem Pool are as follows (note: each step is a seperate function):							*/
  /*    1. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*    2. Output 0x60, writes 0x60 to PnP Address Port (indicates to PnP board that write_data addresses I/O Port Base	*/
  /*	 address bits 15:08).												*/
  /*    3. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*    4. Output 0x03, writes 0x03 (high byte of COM1 I/O port addr, 0x3F8) to board's I/O Port address reg (high byte)*/
  /*    5. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*    6. Output 0x61, writes 0x61 to PnP Address Port (indicates to PnP board that write_data addresses I/O Base Port	*/
  /*	 address register low order byte, bits 07:00).									*/
  /*    7. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*    8. Output 0xF8, writes 0xF8 (low byte of COM1 I/O port addr, 0x3F8) to board's I/O Port address reg (low byte)	*/
  /*    9. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*   10. Output 0x70, writes 0x70 to PnP Address Port (indicates to PnP board that write_data addresses Interrupt	*/
  /*	 Request Level Select 0 register).										*/
  /*   11. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*   12. Output 0x04, writes 0x04 (COM1 Interrupt) to board's Interrupt Request Level Select				*/
  /*   13. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*   14. Output 0x71, writes 0x71 to PnP Address Port (indicates to PnP board that write_data addresses Interrupt	*/
  /*	 Request Type Select 0 register).										*/
  /*   15. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*   16. Output 0x03, writes 0x03 (High - Level triggered Interrupt) to board's Interrupt Request Type Select		*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_31 ;
return ;

}	/*End pnp_probe_fn_30												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           31th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_31 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x60 (I/O Port Base addr bits 15:08) by writing 0x60 to PnP Address Port	*/
oregs->cout_xon_1 	  = 0x60 ;					/*Send I/O Port Addr bit 15:08 to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_32 ;
return ;

}	/*End pnp_probe_fn_31												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  	   32th PnP PROBE FN - Set up to send I/O Port Base addr bits 15:08	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_32 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP I/O Port Base addr bits 15:08 register # has been sent to PnP Address Port				*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP I/O Port bits been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP I/O Port Base Address bits 15:08, now set control signals to address PnP's	*/
/*Write Data Port													*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_33 ;
return ;

}	/*End pnp_probe_fn_32												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           33th PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_33 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, write a 0x03 to Write Data Port - thus causing PnP board to set its ISA I/O Port Base addr bits (15:08) to	*/
/*0x03 (high byte of COM I/O port address 0x3F8), see page 50.								*/
oregs->cout_xon_1 	  = 0x03 ;					/*Send high I/O Port address bits		*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_34 ;
return ;

}	/*End pnp_probe_fn_33												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 34th PnP PROBE FN - start setting up ISA I/O Port Base Address bit 07:00 by	*
 *		       first pointing to PnP Address Port			*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_34 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for PnP CSN to be sent												*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Have I/O Ports bits been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

  /*PnP Board's ISA I/O Port Base Address bits 15:08 = 0x03, point to PnP address port so set write address = 0x61, PnP	*/
  /*I/O Port Base Address bits 07:00											*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_35 ;
return ;

}	/*End pnp_probe_fn_34												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           35th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_35 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x61 (I/O Port Base addr bits 07:00) by writing 0x61 to PnP Address Port	*/
oregs->cout_xon_1 	  = 0x61 ;					/*Send I/O Port Addr bit 07:00 to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_36 ;
return ;

}	/*End pnp_probe_fn_35												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  	   36th PnP PROBE FN - Set up to send I/O Port Base addr bits 07:00	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_36 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP I/O Port Base Addr register # been sent to PnP Address Port						*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has register # been sent?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP I/O Port Base Address bits 07:00, now set control signals to address PnP's	*/
/*Write Data Port													*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_37 ;
return ;

}	/*End pnp_probe_fn_36												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           37th PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_37 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, write a 0xF8 to Write Data Port - thus causing PnP board to set its ISA I/O Port Base addr bits (07:00) to	*/
/*0xF8 (low byte of COM I/O port address 0x3F8), see page 50.								*/
oregs->cout_xon_1 	  = 0xF8 ;					/*Send high I/O Port address bits		*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_38 ;
return ;

}	/*End pnp_probe_fn_37												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 38th PnP PROBE FN - start setting up ISA Interrupt number by	first pointing	*
 *		       to PnP Address Port					*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_38 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for PnP ISA I/O Port Base addr bits (07:00) to be sent								*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Have I/O Ports bits been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

  /*PnP Board's ISA I/O Port Base Address bits 07:00 = 0xF8, point to PnP address port so set write address = 0x70, PnP	*/
  /*Interrupt Request Level (see page 51)										*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_39 ;
return ;

}	/*End pnp_probe_fn_38												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           39th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_39 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x70 (Interrupt Request Level) by writing 0x70 to PnP Address Port		*/
oregs->cout_xon_1 	  = 0x70 ;					/*Send Interrupt Request Addr to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_40 ;
return ;

}	/*End pnp_probe_fn_39												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  	   40th PnP PROBE FN - Set up to send Interrupt Request Level 		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_40 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Interrupt Request Level register # been sent to PnP Address Port					*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP I/O Port bits been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Interrupt Request Level register, now set control signals to address PnP's	*/
/*Write Data Port													*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_41 ;
return ;

}	/*End pnp_probe_fn_40												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           41st PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_41 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, wrt 0x04 to Write Data Port - thus causing PnP board to set its ISA Interrupt # to COM1's Interrupt (0x04)	*/
oregs->cout_xon_1 	  = 0x04 ;					/*Send Interrupt # (see p. 51)			*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_42 ;
return ;

}	/*End pnp_probe_fn_41												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 42th PnP PROBE FN - start setting up ISA Interrupt Request Type by first 	*
 *		       pointing to PnP Address Port				*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_42 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for Interrupt Number to be sent											*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Interrupt Number been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}
  /*PnP Board's ISA Interrupt Request Level (number) has been sent, point to PnP address port so set write address =	*/
  /*0x70, PnP Interrupt Request Type (see page 51)									*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_43 ;
return ;

}	/*End pnp_probe_fn_42												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           43rd PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_43 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x71 (Interrupt Request Type) by writing 0x71 to PnP Address Port		*/
oregs->cout_xon_1 	  = 0x71 ;					/*Send Interrupt Req Type Addr to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_44 ;
return ;

}	/*End pnp_probe_fn_43												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  	   44th PnP PROBE FN - Set up to send Interrupt Request Type 		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_44 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till PnP Interrupt Request Type register # been sent to PnP Address Port						*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Interrupt Request type bits been sent?*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Interrupt Request type register, now set control signals to address PnP's	*/
/*Write Data Port													*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_45 ;
return ;

}	/*End pnp_probe_fn_44												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           45st PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_45 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, wrt 0x03 to Write Data Port - thus causing PnP board to set its ISA Interrupt type to Interrupt 		*/
/*level = high and Interrupt type = level (see page 51)									*/
oregs->cout_xon_1 	  = 0x03 ;					/*Send Interrupt Request Type (see p. 51)	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_46 ;
return ;

}	/*End pnp_probe_fn_45												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 		     46th PnP PROBE FN - Set up to activate PnP board		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_46 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for Interrupt Type to be sent											*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Interrupt type been sent?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}
  /*PnP board has been configured -  I/O Port Address = 0x3F8, Interrupt number = IRQ4 and Interrupt = high, level 	*/
  /*triggered.  Now activate board and set up so MPA can recognize board's presence.  The steps required by the Modem 	*/
  /*Pool are as follows (each step is a seperate function):								*/
  /*    1. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*    2. Output 0x30, writes 0x30 to PnP Address Port (indicates to PnP board that write_data addresses Activate Reg)	*/
  /*    3. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*    4. Output 0x01, writes 0x01 to board's Activate register							*/
  /*    5. Set chan's control signals to 0x111 (address PnP Address Port)	 					*/
  /*    6. Output 0x02, writes 0x02 to PnP Address Port (indicates to PnP board that write_data addresses Configuration	*/
  /*	 Control register)												*/
  /*    7. Set chan's control signals to 0x155 (address PnP Write Data Port)	 					*/
  /*    8. Output 0x02, writes 0x02 to board's Config. Ctrl reg indicating to board to enter "Wait for Key" state 	*/
  /*    9. Clear MPA AUX reg by issuing "Flush" so PnP I/O is disabled hence enabling MPA to "see" newly configured 	*/
  /*	 PnP board.  Also, Clear slot's read toggle (slot->read_toggle == 0x00) and set slot's PnP probe state 		*/
  /*	 (slot->pnp_probe_state = 0xFF) to indicate Probe completed successfully					*/
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_47 ;
return ;

}	/*End pnp_probe_fn_46												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           47th PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_47 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x30 (Activate register) by writing 0x30 to PnP Address Port			*/
oregs->cout_xon_1 	  = 0x30 ;					/*Send Activate Reg Addr to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_48 ;
return ;

}	/*End pnp_probe_fn_47												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  	   48th PnP PROBE FN - Set up to Activate PnP board 			*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_48 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till Activate register # been sent to PnP Address Port								*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has PnP Activate reg # bits been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Activate register, now set control signals to address PnP's Write Data Port	*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_49 ;
return ;

}	/*End pnp_probe_fn_48												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           49th PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_49 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, wrt 0x01 to Write Data Port - thus causing PnP board to be activated (see page 47)				*/
oregs->cout_xon_1 	  = 0x01 ;					/*Send Activate					*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_50 ;
return ;

}	/*End pnp_probe_fn_49												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 	50th PnP PROBE FN - Set up to Place PnP back to "Wait for Key" state	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_50 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for Activate byte to be sent											*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Activate been sent?			*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}
oregs->cout_ctrl_sigs = 0x111 ;						/*Pt MPA to PnP's Address port (0x279)		*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_51 ;
return ;

}	/*End pnp_probe_fn_50												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           51st PnP PROBE FN - Wait till PnP Address Port addressed		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_51 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Address Port being addressed						*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Address	*/
/*Port, now point to PnP Control Register 0x02 (Configuration Control register) by writing 0x02 to PnP Address Port	*/
oregs->cout_xon_1 	  = 0x02 ;					/*Send Cnfg Cntrl Reg Addr to PnP Addr Port	*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_52 ;
return ;

}	/*End pnp_probe_fn_51												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *  	 52nd PnP PROBE FN - Set up to place board in "Wait for Key" state	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_52 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait till Config. Control register # been sent to PnP Address Port							*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has Config. Cntrl reg # bits been sent?	*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

/*PnP Address register now points to PnP Configuration Control register, now set control signals to address PnP's Write */
/*Data Port														*/
oregs->cout_ctrl_sigs = 0x155 ;						/*Pt MPA to PnP's Write Data Port (0xA79)	*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_53 ;
return ;

}	/*End pnp_probe_fn_52												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           53rd PnP PROBE FN - Wait till PnP Write Data Port addressed	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_53 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Delay 4 frames - allow time for MPA to see PnP Write Data Port being addressed					*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 ) /*Has 4 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

/*Have delayed long enough for MPA to "see" control signal setting indicating that output be directed to PnP's Write	*/
/*Data Port, wrt 0x02 to Write Data Port - thus causing PnP board to enter "Wait for Key" state				*/
oregs->cout_xon_1 	  = 0x02 ;					/*Send "Wait for Key" state bit			*/
oregs->cout_flow_cfg  ^= 0x10 ;					/*Initiate xmit'g char				*/
slot->pnp_frame_ctr       = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
slot->pnp_probe_funct     = pnp_probe_fn_54 ;
return ;

}	/*End pnp_probe_fn_53												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 	54th PnP PROBE FN - Set up to Place PnP back to "Wait for Key" state	*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_54 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb      = sspcb ;
iregs	   = iregs ;

/*Wait for "Wait for Key" state bit to be sent										*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )			/*Has "Wait for Key" state been sent?		*/
{									/*If no, check deadman				*/
	if ( pnp_frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )	/*Has deadman time expired?			*/
	{								/*If no, wait & check again later		*/
		return ;
	}								/*If deadman expired, Modem Pool not functioning*/
	pnp_clean_up_ssp (slot, (unsigned char)0xFF) ;			/*Go clean up so can start over			*/
	return ;
}

  /*PnP Board (if exists) is finally configured and initialized.  Modem Pool still in Plug and Play state (i.e. AUX 	*/
  /*reg's PnP bit is set) thus Modem Pool does not check for UART.  Therefore board has yet to be recognized.  Clear	*/
  /*Modem Pool's AUX Reg PnP bit by issuing "Flush".  When Flush issued, Modem Pool starts probing slot for UART and 	*/
  /*since PnP board (if exists) is configured Modem Pool should recognize board and lower slot's break bit.  Driver	*/
  /*should then recognize that Break bit has been lowered and then call "Register Modem Board" Service/function.	*/
  /*Note: 1. Since Modem Pool recognizes PnP board as soon as "Flush" is issued, "Flush" control signals can be set when*/
  /*	     Driver recognizes PnP board and calls "Register Modem" service/function.					*/
  /*	  2. If no PnP Board present then all above logic was executed for naught.  Consequently, functions below delay	*/
  /*	     approximately one minute and then PnP Probe restarts							*/
slot->pnp_probe_state = 0xFF ;						/*Indicate Slot's board has been PnP Configured	*/
slot->read_toggle     = 0x00 ;						/*Set Slot's Read Toogle since Flush clears bit	*/
oregs->cout_ctrl_sigs = 0x188 ;						/*Issue Slot Flush to clear Modem Pool's PnP bit*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
slot->pnp_probe_funct = pnp_probe_fn_55 ;
return ;

}	/*End pnp_probe_fn_54												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *           55th PnP PROBE FN - Wait for Flush Control Bits to be sent		*
 *                                                                             	*
 ********************************************************************************/

static void pnp_probe_fn_55 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/

volatile struct cin_bnk_struct     	*actv_bank ;	/*Ptr to active bank						*/ /*R2*/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb = sspcb ;
oregs = oregs ;

/*Delay 8 frames - allow time for MPA Flush (note: Modem Pool may recognize PnP board while Control signals are set to	*/
/*"Flush")														*/
if ( pnp_frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 ) /*Has 8 frames elapsed?			*/
	return ;							  /*If no, continue probe			*/

  /*Have delayed long enough for MPA to "see" control signal setting indicating to reset Slot.  Consequently, PnP board */
  /*(if exists) has been seen by Modem Pool (i.e. Modem Pool has cleared Break bit).  If not happened already, Driver	*/
  /*should soon recognize board.  Set up and check if PnP board present (i.e. Break bit = 0).  If board present, point	*/
  /*to function that "spins" on Break = 0 (waiting for Driver to recognize PnP board).  If board not present,		*/
  /* (i.e. Break bit = 1), point to function that delays 5 seconds then restart PnP Porbe.				*/
  /*Note: If Driver did recognize PnP board and called "Register Modem Board" then Slot's state changes such that PnP	*/
  /*	  Probing (and function calls) is stopped.  Hence control may never "reach" this function or the next function.	*/
  /*	  On the other hand if Driver did not recognize PnP board instantly, then control is returned here.		*/
pnp_clean_up_ssp (slot, (unsigned char)0x00) ;				/*Go clean up control signals			*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? 	/*Is Bank A active?				*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : 	/*If yes	  				*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; 	/*If no	  					*/
if ( (actv_bank->bank_sigs & 0x0001) == 0 )		    	/*Is PnP Brd presnt (but not regstered by Drvr)?*/
{									/*If yes, go spin waiting for Driver recognition*/
	slot->pnp_probe_funct = pnp_probe_fn_56 ;			/*Point to "spin" function			*/
	return ;
}

  /*Break bit = 1 indicating no PnP board present, point to function that delays 5 seconds prior to restarting PnP Probe*/
slot->pnp_probe_parm  = 0x00 ;						/*Init PnP Board recognition ctr		*/
slot->pnp_probe_state = 0x008F ;					/*Assure slot in initial PnP state indicating no*/
									/* PnP board present in slot			*/
slot->pnp_probe_funct = pnp_probe_fn_57 ;				/*Point to delay function			*/
slot->pnp_frame_ctr   = gregs->gicp_frame_ctr ;				/*Snap shot for timing				*/
return ;

}	/*End pnp_probe_fn_55												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 56th PnP PROBE FN - PnP Board present - "spin" waiting for Driver to 	*
 *		       recognize board						*
 *                                                                             	*
 ********************************************************************************/
/*R2*/
static void pnp_probe_fn_56 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,		
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/

volatile struct cin_bnk_struct     	*actv_bank ;	/*Ptr to active bank						*/ /*R2*/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb = sspcb ;
gregs = gregs ;
oregs = oregs ;

/*Check if PnP board still present											*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Is Bank A active?					*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  					*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  					*/
if ( (actv_bank->bank_sigs & 0x0001) == 0 )		    /*Is PnP Brd present (but not registered by Driver)?*/
	return ;						    /*If yes, spin till Driver recognizes board		*/
	
/*PnP board no longer present - start PnP Probe over									*/
slot->pnp_probe_state = 0x0000 ;					/*Assure slot in initial PnP state indicating no*/
									/* PnP board present in slot			*/
return ;

}	/*End pnp_probe_fn_56												*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 * 57th PnP PROBE FN - PnP Probe complete and PnP board not present, delay 5 	*
 *		       seconds prior to restarting Probe.  Note: delay provides	*
 *		       window where Modem Pool's PnP I/O bit not set thus 	*
 *		       providing window for Modem Pool to recognize non-PnP 	*
 *		       board insertion.						*
 *                                                                             	*
 ********************************************************************************/
/*R2*/
static void pnp_probe_fn_57 (struct ssp_struct *sspcb,		
      			     struct slot_struct *slot,	
      			     volatile struct mpa_global_s *gregs,
			     volatile struct mpa_input_s *iregs,
      			     volatile struct mpa_output_s *oregs	
			    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)						*/
sspcb = sspcb ;
iregs = iregs ;
oregs = oregs ;

/*Delay 2000 frames (~ .5+ seconds) then increment PnP Board recognition delay counter					*/
if ( pnp_frame_delay_chk ( (unsigned short int)2000, gregs, slot ) == 0 ) /*Has ~ 1/2 second elapsed?			*/
	return ;							  /*If no, continue delay			*/

/*Have delayed for ~ .5+ seconds, increment PnP Board recognition delay counter and check if time for Driver to 	*/
/*recognize PnP board has expired											*/
if ( (++slot->pnp_probe_parm) < (5*2) )					/*Has ~5 secs passed & PnP brd not recognized?	*/
{									/*If no, continue delay				*/
	slot->pnp_frame_ctr = gregs->gicp_frame_ctr ;			/*Snap shot for timing				*/
	return ;
}	

/*~5 seconds have expired and PnP Board yet to be registered, assume Modem Pool unable to recognize PnP board as a	*/
/*UART/modem board.  Restart PnP Probe over!										*/
slot->pnp_probe_state = 0x0000 ;					/*Set PnP Probe state = not initialized		*/
return ;

}	/*End pnp_probe_fn_57												*/







/*#stitle ADMIN/DIAG SERVICE - PnP PROBE FUNCTIONS, CLEAN UP SSP REGS#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *     	 ADMIN/DIAG SERVICE - PnP PROBE FUNCTIONS, CLEAN UP SSP REGS (pnp_clean_up_ssp)	*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when Plug and Play Probing aborted or 	*
 *	   completed									*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  pnp_clean_up_ssp  (struct slot_struct      *    slot,			*
 *			          unsigned char		       type)			*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = PnP Probe cmplt - board found		*
 *					o 0xFF = PnP Probe abort			*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	pnp_clean_up_ssp (struct slot_struct      *    slot,
			          unsigned char	               type)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)							*/
volatile struct mpa_input_s       *  	iregs ;		/*Slot's Input Registers					*/
volatile struct cin_bnk_struct     	*actv_bank ;	/*Slot's active bank pointer					*/ /*R2*/
volatile struct mpa_output_s      *  	oregs ;		/*Slot's Output Regsiters					*/

unsigned int			num_chars ;		/*Number of Input Characters to Flush				*/



/************************
 *   START OF CODE	*
 ************************/

/*Restore saved SSP registers												*/
iregs = slot->input ;							/*Slot's input regs ptr				*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? 	/*Is Bank A active?				*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : 	/*If yes	  				*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; 	/*If no	  					*/
oregs = slot->output ;							/*Slot's output regs ptr			*/

/*Assure Input Buffer empty i.e. assure slot's/channel's number of characters = 0					*/
for (num_chars = actv_bank->bank_num_chars; num_chars != 0; --num_chars) /*If number of Input Chars != 0, update	*/
	inc_tail (iregs) ;						     /*Tail Pointer so number characters = 0	*/

/*Restore Driver's SSP Registers											*/
oregs->cout_flow_cfg = (slot->pnp_sv_cout_flow_config & 0x2F) |
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->pnp_sv_cout_cntrl_sig & 0x0237) | 0x0004 ;
oregs->cout_xon_1	= slot->pnp_sv_cout_xon_1 ;
slot->pnp_clean_up      = (void (*)(struct slot_struct      *, unsigned char))0 ;
if ( type != 0x00 )					/*Clean up due to abort?					*/
	slot->pnp_probe_state   = 0x0000 ;		/*If yes, assure slot in initial PnP state			*/
return ;

}					/*End of pnp_clean_up_ssp							*/








/*#stitle INTERNAL/LOCAL FN - PLUG & PLAY FRAME DELAY CHECK (pnp_frame_delay_chk)#*/
/*#page*/
/************************************************************************************************
 *                                                                             			*
 *               	PLUG & PLAY FRAME DELAY CHECK						*
 *                                                                     				*
 * PURPOSE: To determine if caller's number of SSP Frames have elapsed.				*
 *												*
 * NOTES:											*
 *                                                                     				*
 * CALL: unsigned char  pnp_frame_delay_chk  (unsigned short int              frame_delay,	*
 *		  			      struct icp_gbl_struct        *gregs,		*
 *					      struct slot_struct              *slot)		*
 *                                                                     				*
 *			where:	frame_delay = number of elapsed frames				*
 *				gregs  	    = SSP Global reg pointer				*
 *				slot        = Slot Control Block pointer			*
 *                                                                     				*
 *                                                                     				*
 * RETURN: Number of Elapsed Frames exceeded flag:						*
 *		0x00 = Elapsed # Frames NOT exceeded						*
 *		0xFF = Elasped # Frames exceeded						*
 *                                                                     				*
 ************************************************************************************************/

static unsigned char	pnp_frame_delay_chk  (unsigned short int              frame_delay,
	  			  	      volatile struct icp_gbl_struct        *gregs,
				  	      struct slot_struct              *slot
				 	     )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)							*/
unsigned short int	cur_frame_ct ;			/*Current SSP Frame Ctr						*/



/************************
 *   START OF CODE	*
 ************************/

cur_frame_ct = gregs->gicp_frame_ctr ;			/*Get SSP current Frame Counter					*/
if ( cur_frame_ct < slot->pnp_frame_ctr )		/*Did PnP Frame Conter wrap?					*/
	slot->pnp_frame_ctr = 0 ;			/*If yes, adjust						*/
if ( (unsigned short int)(cur_frame_ct - slot->pnp_frame_ctr) < frame_delay )
	return ( (unsigned char)0 ) ;			/*If delay time has not elapsed					*/
return ( (unsigned char)0xFF ) ;			/*If delay time has elapsed					*/

}							/*End of pnp_frame_delay_chk function				*/





/*END*/

/*
** ramp srvc
*/

/********************************************************************************
 *                                                                              *
 *                  REMOTE ACCESS MODEM POOL(RAMP) SOFTWARE SERVICES            *
 *                  ------------------------------------------------            *
 *                                                                              *
 * PURPOSE: To provide a single set of functions for all Drivers that manages   *
 *         RAMP(s).  This single source makes the details of handling RAMP 	*
 *	   hardware transparent to Driver software.  The software is     	*
 *         written such that it can be migrated to any environment or OS.       *
 *                                                                              *
 * NOTE: - Software was originally developed using:                             *
 *           o WATCOM 32 bit compiler                                           *
 *           o Phar Lap 386|linker                                              *
 *           o Debugged/tested in Phar Lap DOS Extender environment             *
 *	 - "MPA" is used throughout source and is the same as "RAMP"		*
 *                                                                              *
 *                                                                              *
 * FILE:   RAMPSRVC.C                                                           *
 * AUTHOR: BILL KRAMER                                                          *
 * REV:    15 (update "MPA_REV" in Header file, RAMP.H, if change rev here)     *
 * DATE:   01/15/98                                                             *
 *                                                                              *
 *                      E Q U I N O X   C O N F I D E N T I A L            	*
 *                      =======================================            	*
 *                                                                              *
 ********************************************************************************/
/*#page*/
/********************************************************************************
 *                                                                              *
 *                               REVISION HISTORY                               *
 *                               ================                               *
 *                                                                              *
 * REV	  DATE				                     COMMENTS           *
 * ===  ========        ======================================================= *
 *  15	01/15/98	1. Added functions to Register Modem to enable 16550 	*
 *			   FIFO							*
 *			2. Added functions to Flush Input/Output Service to 	*
 *			   flush 16550 UART FIFO				*
 *			3. Added functions to Hard Reset to enable 16550 FIFO	*
 *  14	09/30/97	1. Added conditional compilations to accommodate BSDI &	*
 *			   Linux build environments.  Following symbols, if 	*
 *			   defined, indicate respective compilations:		*
 *				RAMP_LINUX = Linux compile			*
 *				RAMP_BSDI  = BSDI compile			*
 *			   NOTE: RAMP_LINUX & RAMP_BSDI are assumed mutually 	*
 *				 exclusive					*
 *  13	09/12/97	1. Increased delay from 4 frames to 0x2000 in "Register	*
 *			   Modem Service" function, mpa_srvc_reg_modem_1_, to 	*
 *			   accomodate "slow" initializing modems.  This problem	*
 *			   was first identified in May '97 and was patched by	*
 *			   Driver developers (see e-mail of 6/3/97 "Modem Pool	*
 *			   & Slow Modems interim resolution")			*
 *  12  06/10/96	1. Added code to determine slots' Power State and save	*
 *			   in Slot Control Block.				*
 *  11	05/23/96	1. Modified function declarations to accomodate UNIX 	*
 *		           compiliers						*
 *  10	05/20/96	1. See MPA_SRVC.DOC for list of changes			*
 *  09	05/10/96	1. See MPA_SRVC.DOC for list of changes			*
 *  08  03/05/96	1. Fixed warning messages when compiled under "streams"	*
 *  07	02/27/96	1. Fix timing problem in "mpa_srvc_reg_modem" init'g	*
 *			   Input/Output baud rate when rate was set to 0	*
 *  06  02/13/96	1. Add new service "mpa_srvc_modify_settings"		*
 *  05  01/19/96	1. Add new service "mpa_srvc_id_functs"			*
 *			2. Updated function "mpa_srvc_reg_ssp" as follows:	*
 *				A. Check if Sys Functions have been established	*
 *				B. Add addresses of Map/Unmap functions to SSPCB*
 *			3. Added new service - "Hard Reset Modem Board"		*
 *			4. Added stub for new service - "Admin/Diag Poll"	*
 *                                                                              *
 * 04A  01/05/95	1. Fixed bug in "async_req_hndlr" to initialize slotcb	*
 *			   such that a "waited" request appears outstanding	*
 *                                                                              *
 *  04  12/21/95	1. Added, setting "internal loop back" control signal in*
 *			   Register Modem Pool Service thus allowing USRobotics	*
 *			   14.4 Sportster Modem to function.			*
 *			2. Added Frame delay to Set Internal & Clear Internal 	*
 *			   Loop Services.  USRobotics 14.4 Sportster needs delay*
 *			   in order to Loop Back data successfully.		*
 *			3. Modified casting of some statements to satisfy UNIX 	*
 *			   compiler						*
 *                                                                              *
 *  03  11/22/95	Added Loop Back Services				*
 *                                                                              *
 *  02	11/08/95        Version used to perform preliminary RAMP hardware	*
 *			validation						*
 *                                                                              *
 *  01  09/07/95	Partially debugged (achival) version with		*
 *			mpa_srvc_reg_modem subfunctions 14 & 15 in tact.	*
 *                                                                              *
 ********************************************************************************/





/*#stitle HEADER FILE DECLARATIONS#											*/
/*#page*/
/*********************************************************
 * HEADER FILE DECLARATIONS                              *
 *********************************************************/

/*SSP Structure names (used to point to Global, Input, and Output registers)            */
/*NOTE: Macros below must be defined prior to inclusion of RAMP.H.  The macros below are*/
/*	necessary since various development environments utilized different structure	*/
/*	definitions for SSP Global, Input and output registers.  The definitions below	*/
/*	refer to structures defined in icp.h					*/
#define    mpa_global_s       icp_gbl_struct		/*Global regs                   */
#define    mpa_input_s        icp_in_struct              	/*Input regs                    */
#define    mpa_output_s       icp_out_struct             	/*Output regs                   */






/*#stitle STRUCTURE DEFINITIONS/DEFINES/LITERALS DECLACARATIONS#*/
/*#page*/
/************************************************************************
 * Structure definitions & defines/literals                             *
 ************************************************************************/





/************************
 *    MACROS/LITERALS	*
 ************************/











/*#stitle PUBLIC DECLACARATIONS#*/
/*#page*/
/************************************************************************************************
 * Public Declarations i.e. Items DEFINED in this compilation Referenced in other Compilations	 *
 ************************************************************************************************/


/****************
 *  Data Areas	*
 ****************/

/****************************************
 *     Data referenced in RAMPADMN.C	*
 ****************************************/

struct  admin_diag_hdr_struct   admin_diag_hdr = {0,{0,0,0,0,0},0,0,0,0,0,0};



/***********************
 *  Service Functions  *
 ***********************/

/*NOTE: See MPA.H	*/




/*#stitle EXTERNAL DECLACARATIONS#*/
/*#page*/
/********************************************************
 * DATA EXTERNAL to this compilation                    *
 ********************************************************/

/*#page*/
/********************************************************
 * Functions EXTERNAL to this compilation		*
 ********************************************************/








/*#stitle FORWARD REFERENCE DECLACARATIONS#*/
/*#page*/
/*********************************************************************************************************************
 * Forward Reference Declarations i.e. Functions that are INTERNAL to this compilation but are forward referenced    *
 *********************************************************************************************************************/

/*Register Modem Board "call back" Functions							*/
static void  mpa_srvc_reg_modem_1  ( 
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1_ (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1_x (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;

static void  mpa_srvc_reg_modem_1A (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1B (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;


static void  mpa_srvc_reg_modem_1C (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1D (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1E (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1F (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1G (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1H (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_1I (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_2  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_3  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_4  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_5  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_5a  (					/*R15*/
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;


static void  mpa_srvc_reg_modem_5b  (					/*R15*/
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_6  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_7  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_8  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_9  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_10 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_11 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_12 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_13 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_16 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_17 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_18 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_19 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_reg_modem_20 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void	mpa_srvc_reg_modem_clean_up (
					     struct slot_struct      *,
					     unsigned char
			  		    ) ;

static void	mpa_srvc_reg_modem_cln_rd (					   /*R9*/
					     struct slot_struct      *,
					     unsigned char
			  		  ) ;


/*Deregister Modem Board "call back" Functions							*/
static void  mpa_srvc_dereg_modem_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   	 ) ;


/*Slot Status "call back" Functions								*/
static void  mpa_srvc_slot_status_1 ( 
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;


/*Initialize (open) UART/Modem "call back" Functions						*/
static void  mpa_srvc_init_modem_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_1A  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_2 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_3 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_4 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_5 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_6 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_7 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;


static void  mpa_srvc_init_modem_8 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_init_modem_9 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
		      		   ) ;

static void  mpa_srvc_init_modem_10 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;

static void  mpa_srvc_init_modem_11 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;

static void  mpa_srvc_init_modem_12 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;

static void  mpa_srvc_init_modem_13 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;

static void	mpa_srvc_init_modem_clean_up (
					      struct slot_struct      *,
					      unsigned char
			  		     ) ;

/*Modify UART/Modem Port Settings "call back" Functions						*/
static void  mpa_srvc_mod_sets_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_2 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_3 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_4 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_5 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_6 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_7 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_8 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void  mpa_srvc_mod_sets_9 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
		      		 ) ;

static void  mpa_srvc_mod_sets_10 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				  ) ;

static void  mpa_srvc_mod_sets_11 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				  ) ;

static void  mpa_srvc_mod_sets_12 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				  ) ;

static void	mpa_srvc_mod_sets_clean_up (
					    struct slot_struct      *,
				            unsigned char
			  		   ) ;

/*Set Internal Loop Back "call back" Functions							*/
static void  mpa_srvc_set_loop_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			         ) ;

static void  mpa_srvc_set_loop_2 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				 ) ;

static void	mpa_srvc_set_loop_clean_up (
					    struct slot_struct      *,
				            unsigned char
			  		   ) ;

/*Clear Internal Loop Back "call back" Functions						*/
static void  mpa_srvc_clr_loop_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			         ) ;

static void  mpa_srvc_clr_loop_2 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			         ) ;

static void	mpa_srvc_clr_loop_clean_up (
					    struct slot_struct      *,
				            unsigned char
			  		   ) ;

/*Close UART/Modem "call back" Functions							*/
static void  mpa_srvc_close_modem_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				    ) ;

/*Flush MPA Slot Input/Output Buffers "call back" Functions					*/
static void  mpa_srvc_flush_1  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			       ) ;

static void  mpa_srvc_flush_2  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			       ) ;

static void  mpa_srvc_flush_3  (				/*R15*/
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			       ) ;


static void  mpa_srvc_flush_4  (				/*R15*/
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			       ) ;

static void	mpa_srvc_flush_clean_up (
					 struct slot_struct          *,
					 unsigned char
		  		        ) ;

/*Hard Reset Modem Board "call back" Functions							*/
static void  mpa_srvc_hard_rst_1  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
		                  ) ;

static void  mpa_srvc_hard_rst_2  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_3  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_4  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_5  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_6  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_6a  (				/*R15*/
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;


static void  mpa_srvc_hard_rst_6b  (				/*R15*/
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_7  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_8  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_9  (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_10 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_11 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_12 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_13 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_14 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_15 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			          ) ;

static void  mpa_srvc_hard_rst_cmplt (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			             ) ;

static void	mpa_srvc_hard_rst_clean_up (
					    struct slot_struct          *,
					    unsigned char
		  		           ) ;

/*Input Error Handler "call back" Functions							*/
static void  mpa_srvc_error_1 	(
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				) ;

/*Start Break "call back" Functions								*/
static void  mpa_srvc_start_break_1   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				) ;

/*Common Break "call back" Functions								*/
static void  mpa_srvc_break_2   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				) ;

static void  mpa_srvc_break_3   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				) ;

static void	mpa_srvc_break_clean_up (
					 struct slot_struct      *,
					 unsigned char
		  		        ) ;

/*Stop Break "call back" Functions								*/
static void  mpa_srvc_stop_break_1   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				) ;

/*MPA<->UART Hardware Flow Control "call back" Functions					*/
static void  mpa_srvc_mpa_flow_1   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_mpa_flow_2   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void  mpa_srvc_mpa_flow_3   (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
				   ) ;

static void	mpa_srvc_mpa_flow_clean_up (
					    struct slot_struct       *,
					    unsigned char
		  		           ) ;

/*Read UART Register "call back" Functions							*/
static void  mpa_srvc_rd_uart_1 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			        ) ;

static void  mpa_srvc_rd_uart_2 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			        ) ;

static void  mpa_srvc_rd_uart_3 (
				     unsigned long,
				     struct marb_struct              *,	
	      			     struct ssp_struct               *,
	      			     struct slot_struct              *,
			    volatile struct icp_gbl_struct        *,
			    volatile struct icp_in_struct               *,
			    volatile struct icp_out_struct              *
			        ) ;

static void	mpa_srvc_rd_uart_clean_up (
					   struct slot_struct          *,
					   unsigned char
		  		          ) ;

static void	mpa_srvc_rd_uart_cln_rd (
					   struct slot_struct          *,
					   unsigned char
		  		          ) ;
/*#page*/
/*Internal/Local Functions									*/
static unsigned char	val_ssp (				 /*Validate SSP Data Area ptr	*/
				 struct ssp_struct      *
				) ;

static unsigned char	val_marb (				/*Validate MARB			*/
				   struct marb_struct      *
				 ) ;

static unsigned short 	async_req_hndlr (			 	  /*Async Req Handler	*/
					 struct marb_struct      *
					) ;

static unsigned char	frame_delay_chk (				/*Frame Delay Check	*/
				unsigned short int,
				volatile struct icp_gbl_struct *,
				struct slot_struct *);


/*#stitle GLOBAL (STATIC) DATA AREA#*/
/*#page*/
/******************************************************************************************
 * Global Data Area i.e. Variables declared in Data Segment but local to this compilation *
 ******************************************************************************************/






/*#stitle ADAPTER SERVICE - IDENTIFY SYSTEM FUNCTIONS (mpa_srvc_id_functs)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                          IDENTIFY SYSTEM FUNCTIONS			       *
 *                                                                             *
 *******************************************************************************/
/*R5*/
extern	unsigned short int	mpa_srvc_id_functs (struct sys_functs_struct      * import_blk) 


{

/************************
 *     LOCAL DATA	*
 ************************/

unsigned long int	i ;			/*Index/counter used to move	*/
						/*Import Block to Admin/diag hdr*/


/************************
 *   START OF CODE	*
 ************************/

/*Assure call not redundant							*/
if ( admin_diag_hdr.init_flag != 0x00 )		/*Fn addrs been prev spec'd?	*/
	return (ERR_ID_FN_REDUNT) ;		/*If yes, redundant call	*/

/*Assure all Function pointers are valid and coherent i.e.			*/
/*  1. Interrupt Function pointers not 0					*/
/*  2. Lock Function pointer either all 0 OR all not 0				*/
if ( (import_blk->blk_ints_fn  == (unsigned long int(*)(void))0) ||
     (import_blk->rstr_ints_fn == (void(*)(unsigned long int))0)  )
	return (ERR_ID_FN_ADDR_BAD) ;		/*If Interrupt Fn Ptrs bad	*/
if ( import_blk->init_lock_fn   == (void *(*)(unsigned long int      *))0 )
{
	if ( (import_blk->attmpt_lock_fn != (unsigned char(*)(void      *))0) ||
     	     (import_blk->unlock_fn	 != (void(*)(void      *))0) )
		return (ERR_ID_FN_ADDR_BAD) ;	/*If Lock Fn ptrs bad		*/
}
else						/*If int_lock function defined	*/
{
	if ( (import_blk->attmpt_lock_fn == (unsigned char(*)(void      *))0) ||
     	     (import_blk->unlock_fn	 == (void(*)(void      *))0) )
		return (ERR_ID_FN_ADDR_BAD) ;	/*If Lock Fn ptrs bad		*/
}

/*Caller's request is valid, Initialize Admin/Diag. Services Header		 */
admin_diag_hdr.init_flag = 0xFFFF ;				/*Indicate init'd*/
for ( i = 0; i < sizeof (struct sys_functs_struct); ++i )	/*Set up & move	 */
	*(((unsigned char *)&(admin_diag_hdr.os_functs))+i) = 	/* System Fn ptrs*/
				*(((unsigned char *)import_blk)+i) ;
admin_diag_hdr.sspcb_lst_lck_hndl = (void      *)0 ;		 /*Init Lock hndl*/
admin_diag_hdr.sspcb_lock	  = (unsigned long int)0 ;	 /*Init prmtve lk*/
admin_diag_hdr.sspcb_link	  = (struct ssp_struct      *)0; /*Init SSPCB hdr*/

/*If system supports/requires LOCKing for muti-processor synchronization, 	*/
/*initialize SSPCB List's LOCK							*/ /*R9*/
if ( admin_diag_hdr.os_functs.init_lock_fn != (void *(*)(unsigned long int      *))0 )
	admin_diag_hdr.sspcb_lst_lck_hndl =		/*System supports Lockng*/
	     (*admin_diag_hdr.os_functs.init_lock_fn)(&admin_diag_hdr.sspcb_lock) ;

/*Return to caller with good status						*/
return (0x00) ;

}						/*End of mpa_srvc_id_functs Function*/







/*#stitle ADAPTER SERVICE - REGISTER SSP (mpa_srvc_reg_ssp)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                          REGISTER SSP                                       *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_reg_ssp (struct ssp_struct            	*  sspcb,
                                	          volatile struct icp_gbl_struct     	*  greg,
						  void 			        *  (* map_fn)( 
									       	       void      * map_parm
									   		     ),
						  void			        * map_parm,
						  void			           (* unmap_fn)(
									       	       void      * unmap_parm
									                       )
						 )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*Variables used to manage List of SSPCBs					*/
unsigned long int	svd_int_msk ;			/*Int Msk prior to Dsabl*/
struct ssp_struct     	*sspcb_srch_ptr ;		/*Ptr to SSPCB in List	*/
struct ssp_struct     	*current_hd ;			/*Ptr to SSPCB at Lst HD*/

/*General Working Variables							*/
unsigned long int	silent_death_ctr ;		/*Silent death counter	*/
unsigned short int	frame_ctr ;			/*SSP Frame Counter	*/
unsigned short int	i ;				/*Loop counter		*/


/************************
 *   START OF CODE	*
 ************************/

/*Validate caller's parameters							*/
if ( admin_diag_hdr.init_flag != 0xFFFF )	/*Have Fn addrs been spec'd?	*/ /*R5*/
	return (ERR_REG_SSP_NO_FNS) ;		/*If no, error			*/
if ( map_fn == (void      *(*)(void      *))0 )	/*Are function addrs consistent?*/
{
	if ( unmap_fn != (void (*)(void      *))0 )				   /*R8*/
		return (ERR_REG_SSP_FN_ADR_BAD) ;	  /*If no		*/
}
else
{
	if ( unmap_fn == (void (*)(void      *))0 )				   /*R8*/
		return (ERR_REG_SSP_FN_ADR_BAD) ;
}
for (silent_death_ctr = 0xFFFFF, frame_ctr = greg->gicp_frame_ctr; /*Set up & chk*/
     (frame_ctr == greg->gicp_frame_ctr) && 			   /*if Global	 */
     (silent_death_ctr != 0) ;					   /*reg ptr sane*/
     --silent_death_ctr) ;
if (silent_death_ctr == 0)					/*Is Ptr OK?	*/
	return (ERR_REG_SSP_GREG) ;				/*If no, err ret*/
if ( (sspcb->signature == sspcb) || (sspcb->in_use == 0xFF) )
	return (ERR_REG_SSP_SSP_AREA) ;				/*SSP ptr bad	*/

/*Gain access to SSPCB list thus preventing Service functions and Diag/Admin 	*/
/*Poll function from accessing SSPCB list while determining if caller's SSPCB 	*/
/*already in list and while SSPCB being added to list				*/
while ( (mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk)) != 0xFF ) {}	/*Spin till	*/ /*R9*/
							      	/*access granted*/
for ( silent_death_ctr = 0x200, sspcb_srch_ptr = admin_diag_hdr.sspcb_link ;	   /*R9*/
     (sspcb_srch_ptr   != (struct ssp_struct      *)0) &&
     (silent_death_ctr != 0) 			       ;
     sspcb_srch_ptr = sspcb_srch_ptr->sspcb_link,
     --silent_death_ctr				      )	/*Chk if caller's SSPCB	*/
{							/*already in list	*/
	if ( sspcb == sspcb_srch_ptr )			/*SSPCB in list ?	*/
	{						/*If yes, error		*/
		mpa_fn_relse_sspcb_lst (svd_int_msk) ;
		return (ERR_REG_SSP_SSP_AREA) ;		/*SSP ptr bad		*/
	}
}										   /*R9*/

/*Caller's SSPCB valid, Initialize caller's SSPCB				*/
/*NOTE: SSPCB List Locked							*/
for (i = 0; i < sizeof(struct ssp_struct); ++i )
	*(((unsigned char *)sspcb)+i) = 0 ;
sspcb->in_use     = 0xFF ;					/*In Use flag	*/
sspcb->rev        = MPA_REV ;					/*Software rev	*/
sspcb->admin_rev  = MPA_ADMIN_DIAG ;				/*Admin rev	*/  /*R9*/
sspcb->signature  = sspcb ;					/*Signature	*/
sspcb->map_fn     = map_fn ;					/*Sv MAP Fn addr*/
sspcb->map_parm   = map_parm ;					/*Sv MAP FN parm*/
sspcb->unmap_fn   = unmap_fn ;					/*Sv UNMAP addr	*/
sspcb->global     = greg ;					/*SSP Global reg*/
for (i = 0; i < ENGS_MPA; ++i)
	sspcb->mpa[i].base_chan = 0xFF ;			/*Eng. base chan*/

/*Set up and add SSPCB to Administrative & Diagnostic Service Header's Chain of	*/
/*Registered SSPCBs 								*/
/*NOTE: SSPCB List Locked							*/
current_hd 		  = admin_diag_hdr.sspcb_link ;	/*Addr of SSPCB @ Lst HD  */
admin_diag_hdr.sspcb_link = sspcb ;			/*Caller's SSPCB to Lst HD*/
sspcb->sspcb_link 	  = current_hd ;		/*Chain list to new SSPCB */

/*Release SSPCB List and return to caller					  */
mpa_fn_relse_sspcb_lst (svd_int_msk) ;						    /*R9*/
return (0x00) ;

}						/*End of mpa_srvc_reg_ssp Function*/









/*#stitle ADAPTER SERVICE - REGISTER MPA (mpa_srvc_reg_mpa)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                          REGISTER MPA                                       *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_reg_mpa (struct ssp_struct      *  sspcb,
                                	  	  unsigned char		    chan_num,
					  	  struct slot_struct        slot[],
					  	  volatile struct icp_in_struct      *  ireg)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*Variables used in accessing SSP registers					*/
unsigned short int		lmx_id ;		/*LMX ID frm off-lne TDM*/

/*MPA hardware variables							*/
unsigned char			mpa_id ;		/*MPA ID		*/
unsigned char			num_engs ;		/*MPA's # Engs (1,2,4)	*/
unsigned char			sp_mul_msk ;		/*SSP's LMX Scan sp mask*/
unsigned char			sp_mul_set ;		/*SSP's LMX Scan sp setg*/
unsigned int			slots_per_eng ;		/*# Slots per MPA Eng	*/
unsigned char			rev ;			/*Modem Pool's rhdwr REV*/ /*R12*/

/*Variables used to gain access to SSPCB List					*/
unsigned long int		svd_int_msk ;		/*Int Msk prior to Dsabl*/

/*SSPCB (SSP Control Block), MPACB (MPA Control Block) and SLOTCB (Slot 	*/
/*Control Block) variables							*/
unsigned int			mpa_base_index ;	/*1st MPA's MPACB index	*/
							/*or # 1st MPA on Ring	*/
unsigned int			mpa_index ;		/*Working MPA Array indx*/
unsigned char			eng_num ;		/*MPA current Engine #	*/
struct slot_struct     		*slot_member ;		/*Eng's Slot Member	*/
volatile struct icp_in_struct       *slot_iregs ;		/*Slot's SSP Input reg	*/
volatile struct icp_out_struct      *slot_oregs ;		/*Slot's SSP Output reg	*/
struct mpa_struct      		*actv_eng;		/*MPA member be'g init'd*/

/*General working variables							*/
unsigned int			i ;			/*Loop counter		*/
unsigned int			j ;			/*Loop counter		*/



/************************
 *   START OF CODE	*
 ************************/

/*Validate caller's parameters							*/
if ( (sspcb->in_use    != 0xFF)     ||			/*Does caller's SSP	*/
     (sspcb->rev       != MPA_REV)  ||			/* Control Block apper	*/
     (sspcb->signature != sspcb) )			/*  sane?		*/
	return (ERR_REG_MPA_SSP_AREA) ;			/*If no, exit		*/
if ( (chan_num & 0xCF) != 0 )				/*Is caller's chan # OK?*/
	return (ERR_REG_MPA_CHAN_NUM) ;			/*If no, exit		*/
for ( i = 0; i < SLOT_ARRAY_SZ; ++i)			/*Is caller's Slot array*/
	if (slot[i].in_use == 0xFF)			/* in use?		*/
		return (ERR_REG_MPA_SLOT_ARRY) ;	/*If yes, exit		*/

/*Set up and get MPA's number of engines					*/
lmx_id = ireg[1].cin_tdm_early ;			/*Get off-line TDM w/ID	*/
if ( (lmx_id & 0xC400) != 0xC000 )			/*Is TDM sane?		*/
	return (ERR_REG_MPA_IN_REG) ;			/*If no,caller's ptr bad*/
mpa_id = (unsigned char)((unsigned int)(lmx_id & 0x01FE) >> 1); /*Develop MPA ID*/
if ( (mpa_id != 0x08) &&				/*Is MPA ID valid?	*/
     (mpa_id != 0x09) &&
     (mpa_id != 0x0B) )
	return (ERR_REG_MPA_ID_BAD) ;			/*If MPA ID bad		*/
num_engs = ((mpa_id & 3) == 0) ? 1 : 			/*Develop # MPA engs 	*/
	   ( ((mpa_id & 3) == 1) ? 2 : 4) ;

/*Set up and validate that MPA & its engines have not been previously registered*/
mpa_base_index = chan_num >> 4 ;			/*Index to 1st MPACB	*/
for (mpa_index = mpa_base_index, i = 0;			/*Loop to check if SSP's*/ 
     i < num_engs; 					/* MPA & Engines already*/
     ++mpa_index, ++i)					/*  registered		*/
{
	if ( mpa_index >= ENGS_MPA )			/*Did exceed MPA/Eng Array*/
		return (ERR_REG_MPA_ENG_OFL) ;		/*If yes, config error	*/
	if ( sspcb->mpa[mpa_index].base_chan != 0xFF )	/*Is engine alrdy def'd?*/
		return (ERR_REG_MPA_ENG_ALLOC) ;	/*If yes, return w/error*/
}

/*Caller's parms & MPA Hardware are OK, initialize SSP's speed multiplier based	*/
/*on MPA's starting position in Ring and MPA's number of Engines		*/
switch (mpa_base_index)					/*Position of 1st MPA	*/
{							/* channel on Ring:	*/
	case 0:						/*MPA @ LMX 0		*/
		switch (num_engs)
		{
			case 1:				/*SP X, 1 eng @ LMX 0	*/
				sp_mul_msk = 0xFC ;
				sp_mul_set = 0x00 ;
				break ;
			case 2:				/*SP X, 2 engs @ LMX 0	*/
				sp_mul_msk = 0xF0 ;
				sp_mul_set = 0x05 ;
				break ;
			case 4:				/*SP X, 4 engs @ LMX 0	*/
				sp_mul_msk = 0x00 ;
				sp_mul_set = 0xAA ;
				break ;
			default:
				return (ERR_REG_MPA_ENG_OFL) ;
		}
		break ;
	case 1:						/*MPA @ LMX 1		*/
		switch (num_engs)
		{
			case 1:				/*SP X, 1 eng @ LMX 1	*/
				sp_mul_msk = 0xF3 ;
				sp_mul_set = 0x00 ;
				break ;
			case 2:				/*SP X, 2 engs @ LMX 1	*/
				sp_mul_msk = 0xC3 ;
				sp_mul_set = 0x14 ;
				break ;
			default:
				return (ERR_REG_MPA_ENG_OFL) ;
		}
		break ;
	case 2:						/*MPA @ LMX 2		*/
		switch (num_engs)
		{
			case 1:				/*SP X, 1 eng @ LMX 2	*/
				sp_mul_msk = 0xCF ;
				sp_mul_set = 0x00 ;
				break ;
			case 2:				/*SP X, 2 engs @ LMX 2	*/
				sp_mul_msk = 0x0F ;
				sp_mul_set = 0x50 ;
				break ;
			default:
				return (ERR_REG_MPA_ENG_OFL) ;
		}
		break ;
	case 3:						/*MPA @ LMX 3		*/
		switch (num_engs)
		{
			case 1:				/*SP X, 1 eng @ LMX 3	*/
				sp_mul_msk = 0x3F ;
				sp_mul_set = 0x00 ;
				break ;
			default:
				return (ERR_REG_MPA_ENG_OFL) ;
		}
		break ;
	default:					/*Should not occur, but	*/
		return (ERR_REG_MPA_CHAN_NUM) ;		/* just in case		*/
}
sspcb->sv_sp_mul_msk = sp_mul_msk ;			/*Sv for sanity chk'g	*/
sspcb->sv_sp_mul_set = sp_mul_set ;			/*Sv for sanity chk'g	*/
sspcb->global->gicp_scan_spd &= sp_mul_msk ;		/*Clr MPA Eng's SP X bits*/
sspcb->global->gicp_scan_spd |= sp_mul_set ;		/*Set MPA Eng's SP X bits*/

/*Background caller's Slot array area to 0					*/
for (i = 0; i < (sizeof(struct slot_struct)*SLOT_ARRAY_SZ); ++i )
	((unsigned char *)slot)[i] = 0 ;

/*Prepare to initialize each engine's MPA Control Block				*/
slots_per_eng = SLOT_ARRAY_SZ/num_engs ;		/*# slots per MPA Eng.	*/
eng_num       = 0 ;					/*Initialize Eng. #	*/
slot_member   = slot ;					/*Eng's 1st Slot member	*/
slot_iregs    = ireg ;					/*Eng's 1st Slot inp reg*/
slot_oregs    = (volatile struct icp_out_struct      *)(((unsigned char      *)ireg) + 
		((sizeof(struct icp_in_struct)*64))) ;
rev	      = (ireg->cin_tdm_early >> 1) & 0x0F ;	/*Get Modem Pool's Rev	*/ /*R12*/

/*For caller's SSPCB, prepare to initialize each eng's MPA Control Block (MPACB)*/
/*but first gain access to SSPCB List thus Blocking Admin/Diag Poll function  	*/
/*from traversing and potentually accessing a partially initialized MPACB	*/ /*R9*/
while ( (mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk)) != 0xFF ) {} /*Spin till access*/
for (mpa_index = mpa_base_index, i = 0;			/*Loop to init. SSP's	*/
     i < num_engs; 					/* MPA/Eng and Slot	*/
     ++mpa_index,					/*  array		*/
     ++eng_num,
     ++i)
{ 
     /*Initialize SSP's MPACB							*/
	actv_eng = &(sspcb->mpa[mpa_index]) ;		/*Point to MPA/Eng memb	*/
	actv_eng->base_chan      = chan_num ;		/*MPA's Ring Position 	*/
	actv_eng->mpa_num_engs   = num_engs ;		/*MPA's Number of Engs.	*/
	actv_eng->input_base     = ireg ;		/*MPA's 1st Input reg	*/
	actv_eng->slot_array_hd  = slot ;		/*Hd of MPA's Slot Array*/
	actv_eng->mpa_hrdwre_rev = rev ;		/*Save MPA's Hrdwre REV	*/ /*R12*/
	actv_eng->eng_num        = eng_num ;		/*MPA array member Eng #*/
	actv_eng->eng_head       = slot_member ;	/*MPA Eng's 1st Slot mem*/

     /*Initialize MPA (Engine's) Slot Members AND each slot's Internal Loop Back*/
     /*Control signal (SSP Output Control Signal register) thus stopping "flood"*/
     /*of UART Interrupts received by RAMP.  This is necessary since some 	*/
     /*Modem's interrupt line is "floating" until Slot is initialized.		*/
	for (j = 0; j < slots_per_eng;			/*Loop to init Eng's	*/
	     *(unsigned long int      *)&slot_member += sizeof(struct slot_struct), ++j)
	{						/* Slot members		*/
		slot_member->in_use    = 0xFF ;		/*Set Mem in use flag	*/
		slot_member->alloc     = 0xFF ;		/*Indicate member init'd*/
		slot_member->signature = slot_member ;	/*Member's signature	*/
		slot_member->input     = slot_iregs+j ;	/*Slot's SSP Input regs	*/
		slot_member->output    = slot_oregs+j ;	/*Slot's SSP Output regs*/
/*R9*/		slot_member->global    = sspcb->global;	/*Slot's Global Reg base*/
		slot_member->chan_num  = chan_num+(i * 0x10)+j; /*Slot's Chan #	*/
/*R9*/		slot_member->slot_state      = 0x00 ;	/*PnP slot state = empty*/
/*R9*/		slot_member->pnp_probe_state = 0x0000 ;	/*PnP Probe state = 	*/
							/* Probing not init'd	*/
	     /*Initialize Slot's Internal Loop Back Control Signals		*/
/*R4*/		(slot_oregs+j)->cout_ctrl_sigs = ((slot_oregs+j)->cout_ctrl_sigs & 0x02BF) | 0x008C ;
	}						/*End of FOR loop init'g*/
							/*an Eng's Slot member	*/
	*(unsigned long      *)&slot_iregs  += 		/*Nxt Eng(LMX) Input regs*/
		(0x10 * sizeof(struct icp_in_struct)) ;
	*(unsigned long      *)&slot_oregs  += 		/*Nxt Eng(LMX) Output regs*/
		(0x10 * sizeof(struct icp_out_struct)) ;
}							/*End of FOR loop init'g*/
							/*SSP MPA/Eng member(s)	*/

/*SSP MPACBs and corresponding Slot members initialized, release SSPCB List and */
/*return to caller with good status						*/
mpa_fn_relse_sspcb_lst (svd_int_msk) ;						  /*R9*/
return (0x00) ;

}					/*END of mpa_srvc_reg_mpa function	*/







/*#stitle ADAPTER SERVICE - DEREGISTER MPA (mpa_srvc_dereg_mpa)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                          DE-REGISTER MPA                                    *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_dereg_mpa (struct ssp_struct      *       sspcb,
                                	  	  unsigned char		           chan_num,
					  	  struct slot_struct      *      * slot_ret_parm)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSPCB (SSP Control Block), MPACB (MPA Control Block) and SLOTCB (Slot 	*/
/*Control Block) variables							*/
unsigned int		mpa_index ;			/*MPA Array member	*/
struct slot_struct     	*slot_member ;			/*MPA's Slot Member	*/

/*MPA hardware variables							*/
unsigned int		num_engs ;			/*MPA's # Engs (1,2,4)	*/

/*Variables used to gain access to SSPCB List					*/
unsigned long int	svd_int_msk ;			/*Int Msk prior to Dsabl*/

/*Variables used to clean up SLOTCB						*/
unsigned int			slots_per_eng ;		/*# slots per eng	*/
unsigned char			eng_slot_num_base ;	/*Multi-eng Slot # base	*/
unsigned char			eng_slot_num_offset ;	/*Multi-eng Slot # offst*/
struct marb_struct      *	can_marb ;		/*Canceled MARB addr	*/

/*General working variables							*/
unsigned char			status ;		/*Funct. ret status	*/
unsigned short int		short_status ;		/*Funct. ret status	*/
unsigned int			i ;			/*Loop counter		*/



/************************
 *   START OF CODE	*
 ************************/

/*Validate caller's parameters							*/
if ( (status = val_ssp (sspcb)) != 0x00 )		/*Is caller's parm OK?	*/
	return (ERR_DREG_MPA_SSP_AREA) ;		/*If no, exit		*/
if ( (chan_num & 0xCF) != 0 )				/*Is caller's chan # OK?*/
	return (ERR_DREG_MPA_CHAN_NUM) ;		/*If no, exit		*/
mpa_index   = chan_num >> 4 ;				/*Index to 1st MPACB	*/
if ( sspcb->mpa[mpa_index].base_chan == 0xFF )		/*Chan # pt to valid MPA*/
	return (ERR_DREG_MPA_BAD_ENG) ;			/*If no, exit		*/
if ( sspcb->mpa[mpa_index].eng_num != 0x00 )		/*Chan # pt to 1st eng	*/
	return (ERR_DREG_MPA_BAD_ENG) ;			/*If no, exit		*/

/*Init variables so can free SSP MPA Array members and cooresponding Slot area	*/
slot_member   = sspcb->mpa[mpa_index].slot_array_hd;	/*MPA's Slot Array base	*/
num_engs      = sspcb->mpa[mpa_index].mpa_num_engs ;	/*MPA # Engs.		*/
slots_per_eng =  (num_engs == 1) ? 16 :			/*Get # slots per engine*/ /*R9*/
		((num_engs == 2) ?  8 : 4 ) ;

/*Prior to "cleaning up" each MPA Slot, gain access to SSPCB List thus Blocking */
/*Admin/Diag Poll function from traversing and potentually accessing a SLOTCB	*/
/*that is in process of being cleaned up					*/ /*R9*/
while ( (mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk)) != 0xFF ) {} /*Spin till access granted*/
for ( eng_slot_num_base = 0, eng_slot_num_offset = 0, i = 0;
      i < SLOT_ARRAY_SZ;
      ++eng_slot_num_offset, ++i)
{
	if ( eng_slot_num_offset >= slots_per_eng )	/*If cleaning up next	*/
	{						/* eng's Slot's		*/
		eng_slot_num_base   += 0x10 ;		/*Get nxt eng's 1st slot*/
		eng_slot_num_offset  = 0 ;		/* number		*/
	}
	if (slot_member[i].slot_state != 0x00)		/*Is Slot Empty?	*/
	{						 /*If no, go cancel any	*/
		short_status = mpa_srvc_can_marb (sspcb, /* outstanding MARB	*/
				 	  (unsigned char)(chan_num+eng_slot_num_base+eng_slot_num_offset),
					  (struct marb_struct      *      *)&can_marb) ;
	}
	else						/*Empty Slot, go cancel	*/
	{						/* PnP Probing		*/
		if ( (sspcb->pnp_probe_ena == 0xFF) 		&&
/*R10*/		     (slot_member[i].pnp_probe_state != 0x0000) &&
     		     (slot_member[i].pnp_clean_up    != (void (*)(struct slot_struct      *, unsigned char))0) )
		{					/*If PnP Probe active	*/
			(*slot_member[i].pnp_clean_up)(&slot_member[i], (unsigned char)0xFF) ;
		}
	}
	slot_member[i].in_use          = 0x00 ;		/*Free Slot member	*/
	slot_member[i].alloc           = 0x00 ;		/*Elm. uninit'd		*/
	slot_member[i].slot_state      = 0x00 ;		/*Indicate no Modem prsnt*/
	slot_member[i].init_state      = 0x00 ;		/*MODEM/UART Closed	*/
	slot_member[i].pnp_probe_state = 0x0000 ;	/*PnP not initialized	*/
}

/*Set up & free all Engines associated with caller's MPA			*/
for (i = 0; i < num_engs; ++mpa_index, ++i)		/*Loop to free MPA Engs	*/
	sspcb->mpa[mpa_index].base_chan = 0x0FF ;	/*Indicate MPA (Eng) not*/
							/* on Ring		*/

/*Caller's MPA is de-registered, release SSPCB List and return pointer to freed	*/
/*Slot area									*/
mpa_fn_relse_sspcb_lst (svd_int_msk) ;						 /*R9*/
*slot_ret_parm = slot_member ;
return (0x00) ;

}					/*END of mpa_srvc_dereg_mpa function	*/







/*#stitle SLOT SERVICE - REGISTER MODEM BOARD (mpa_srvc_reg_modem)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                  REGISTER MODEM BOARD - SERVICE ENTRY POINT                 *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_reg_modem (struct marb_struct       *marb)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_REG_MODEM_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_reg_modem_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_REG_MODEM ) /*Valid "Call Back"?	*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_REG_MODEM_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/

}			/*END of mpa_srvc_reg_modem Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *             REGISTER MODEM BOARD - 1st FUNCTION, INIT SLOT SSP REGS.	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1  ( unsigned long		        caller_parm,
 				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*Variables used to determine MPA Slot Power States				    */ 	/*R12*/
volatile struct icp_in_struct      * mpa_1st_iregs ;	/*MPA's 1st Slot Input regs */
struct slot_struct      *  	mpa_1st_slot ;		/*MPA's 1st SLOTCB	    */
unsigned char			slot_pwr_state ;	/*MPA SLOT PWR State	    */

/*Variables used in accessing SSP registers					*/
volatile struct cin_bnk_struct      *	actv_bank ;	/*Ptr to active bank	*/
unsigned short int		ssp_ushort_var ;	/*Used to modify 16 bit	*/
							/*SSP registers		*/
unsigned char			ssp_uchar_var ;		/*Used to modify 8 bit	*/
							/*SSP registers		*/

/*Variables used to gain access to SSPCB List					*/
unsigned long int		svd_int_msk ;		/*Int Msk prior to Dsabl*/

/*General working variables							*/
unsigned int			i ;			/*Loop counter		*/
unsigned int			j ;			/*Loop counter		*/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*For request's MPA, update the Power Switch state for each Slot Member	     			*/ 	/*R12*/
mpa_1st_iregs = sspcb->mpa[(marb->slot_chan >> 4)].input_base ;   /*Pt to MPA's base Input regs	*/
mpa_1st_slot  = sspcb->mpa[(marb->slot_chan >> 4)].slot_array_hd; /*Pt to 1st SLOT to update	*/
for ( i = 0; i < 16; i = i + 4 )				  /*Update MPA's SLOT PWR States*/
{
	actv_bank = ( ((mpa_1st_iregs+i)->cin_locks & 0x01) == 0x00 )		?
      		(volatile struct cin_bnk_struct      *)&((mpa_1st_iregs+i)->cin_bank_a) :
      		(volatile struct cin_bnk_struct      *)&((mpa_1st_iregs+i)->cin_bank_b) ;
	slot_pwr_state = ( ((actv_bank->bank_sigs) & 0x0800) == 0x0000)  ?
		(unsigned short int)0x0000 : (unsigned short int)0x00FF 	;
	for ( j = 0; j < 4; ++j )				  /*Loop to update SLOT PWR State*/
		(mpa_1st_slot+i+j)->power_state = slot_pwr_state ;
}													/*R12*/

/*Complete Request's validation							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_REG_MODEM_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0x00 )				/*Is Modem already reg?	*/
{							/*If yes, error		*/
	marb->srvc_status = ERR_REG_MODEM_REG ;		/*Indicate error	*/
	return ;					/*Exit			*/
}
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  */
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  */
if ( (actv_bank->bank_num_chars) != 0 )		/*Is Input Queue empty?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_REG_MODEM_IN_Q;		/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, gain access to SSPCB List thus blocking Admin/Diag Poll*/  /*R9*/
/*Function from traversing and potentually accessing a SLOTCB that is in process */
/*of being initialized.								*/
while ( (mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk)) != 0xFF ) {} /*Spin till access granted*/
slot->type = (slot->pnp_probe_state != 0xFF) ? 0x00 : 0x01  ; /*Indicate Board type	*/
if ( (sspcb->pnp_probe_ena  == 0xFF)   &&		      /*Is Slot PnP Probing	*/
     (slot->pnp_probe_state != 0x0000) &&		      /*  active? 	        */
     (slot->pnp_clean_up    != (void (*)(struct slot_struct      *, unsigned char))0) )
{							      /*If yes, cancel Probing  */
	(*slot->pnp_clean_up)(slot, (unsigned char)0xFF) ;
}

/*Save SSP registers that will be used to Register Modem Board			*/
slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
slot->sv_cout_xon_1       = oregs->cout_xon_1 ;

/*Establish "clean up" function in case	De-register Modem request handled 	*/
/*asynchronously								*/
slot->clean_up            = (void(*)(struct slot_struct      *, unsigned char))mpa_srvc_reg_modem_clean_up ;

/*Initialize MARB's Slot Control Block						*/
slot->slot_state      = 0x80 ;				/*Modem be'g registered	*/
slot->io_base         = 0x00 ;				/*I/O base = invalid	*/
slot->init_state      = 0x00 ;				/*MODEM/UART closed	*/
slot->read_toggle     = 0x00 ;				/*UART I/O Read toggle	*/
slot->flow_setting    = marb->sup_uchar0 ;		/*Save flow setting	*/
slot->num_bits	      = marb->sup_uchar1 ;		/*Save # bits setting	*/
slot->stop_bits	      = marb->sup_uchar2 ;		/*Save # stop bits	*/
slot->parity   	      = marb->sup_uchar3 ;		/*Save parity setting	*/
slot->baud	      = marb->sup_ushort ;		/*Save baud setting	*/
slot->break_state     = 0x00 ;				/*Indicate Break = off	*/
slot->loop_back       = 0x00 ;				/*Loop Back state = OFF	*/
slot->pnp_probe_state = 0x0000 ;			/*PnP not initialized	*/
mpa_fn_relse_sspcb_lst (svd_int_msk) ;			/*Release SSPCB List	*/ /*R9*/

if ( marb->scratch_area == (unsigned char      *)0 )	/*Is scratch area spec'd*/
	for ( i = 0; i < SLOT_SCRATCH_SZ; ++i )		/*If no, clear (0) slot	*/
		slot->rscratch[i] = (unsigned char)0x00; /* area		*/
else
	for ( i = 0; i < SLOT_SCRATCH_SZ; ++i )		/*If yes, copy area	*/
		slot->rscratch[i] = marb->scratch_area[i] ;
for ( i = 0; i < SLOT_SCRATCH_SZ; ++i )			/*Clear scratch spec'd	*/
	slot->oscratch[i] = (unsigned char)0x00 ;	/*during Init/Open Req	*/

/*Initialize Slot's SSP channel registers:					*/
/*	*** Transmint & Receive char attributes (8,N,1)				*/
ssp_ushort_var  = (iregs->cin_char_ctrl & 0xFFE0) ;	/*Set up SSP Input to 	*/
ssp_ushort_var |= 0x03 ;				/*recv 8 bits, no parity*/
iregs->cin_char_ctrl = ssp_ushort_var ;
oregs->cout_char_fmt = 0x03 ;				/*SSP output = 8, none	*/

/*	*** Transmint & Receive Baud Rate = Top rate				*/
iregs->cin_baud = 0x7FFF ;				/*Input baud rate = TOP	*/
oregs->cout_baud = 0x7FFF ;			/*Output baud rate = TOP*/
while ( 1 == 1 )					/*Loop to reset SSP 	*/ /*R7*/
{							/* internal baud ctrs	*/
	while ( gregs->gicp_chnl == slot->chan_num ) {}	/*  assures baud setting*/
	iregs->cin_baud_ctr &= 0xC000 ;			/*   takes affect imedly*/
	oregs->cout_int_baud_ctr = 0 ;
	while ( gregs->gicp_chnl == slot->chan_num ) {}	
	if ( ((iregs->cin_baud_ctr & 0x3FFF) == 0) &&
	       (oregs->cout_int_baud_ctr   == 0) )
			break ;
}

/*	*** Set Xtra DMA (since running at TOP baud rate) & Send XON Char	*/
ssp_uchar_var  = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;
oregs->cout_flow_cfg = ssp_uchar_var ;

/*	*** Flow Control, MPA-to-SSP = "MUX not Connected"			*/
/*	                  SSP-to-MPA = Output Cntrl sig 3, 			*/
iregs->cin_susp_output_lmx |= 0x20 ;			/*MUX not Connected	*/
iregs->cin_q_ctrl |= 0x20 ;				/*Set Input Flow Control*/

/*	*** Clear internal/external Loop backs					*/
oregs->cout_lmx_cmd &= 0xFC ;			/*Clear loop backs	*/

/*Change Control Signals so UART gets properly initialize			*/
oregs->cout_ctrl_sigs = 0x77 ;				/*Set UART's cntrl sigs */
slot->frame_ctr  = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->actv_srvc  = MARB_ACTV_SRVC_REG_MODEM ;		/*Indicate outstnd'g req*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_reg_modem_1_ ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}		/*End of REG MODEM BOARD - 1 Function, mpa_srvc_reg_modem_1_	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1_ FUNCTION, CHANGE CONTROL SIGNALS (Forcing MPA  *
 *					  to Write/initialize MCR)	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1_ ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
iregs       = iregs ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x2000, gregs, slot ) == 0 )	 /*R13*/

	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" 1st control signal setting, change	*/
/*output Control Signals thus forcing MPA to write to UART MCR (thus initialize	*/
/*MCR register).  Note: assumes	board is a Modem board				*/
oregs->cout_ctrl_sigs = 0x44 ;				/*Set UART's cntrl sigs */
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1_x ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}		/*End of REG MODEM BOARD - 1_ Function, mpa_srvc_reg_modem_1_	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1_x FUNCTION, RESET MPA  			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1_x ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
iregs       = iregs ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting - MPA has	*/
/*initialized UART's MCR (assuming Modem board), issue MPA RESET		*/
oregs->cout_ctrl_sigs = 0x188 ;				/*Issue Slot RESET to 	*/
slot->read_toggle     = 0x00 ;				/*assure state of MPA's	*/
							/*Read Toggle		*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1A ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}		/*End of REG MODEM BOARD - 1_x Function, mpa_srvc_reg_modem_1_x	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1A FUNCTION, ADDRESS UART's SPR REGISTER 	       *
 *					  (Determine if Board has UART)	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1A ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
iregs       = iregs ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*8 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that Slot should be reset, set up to address SPR register so can write to it	*/
oregs->cout_ctrl_sigs = 0x177 ;				/*Addr UART's SPR	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1B ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}		/*End of REG MODEM BOARD - 1A Function, mpa_srvc_reg_modem_1A	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1B FUNCTION, ATTEMPT TO WRITE TO UART's SPR SO    *
 *			           	  CAN ATTEMPT TO READ BACK & DETERMINE *
 *					  IF BOARD HAS UART	               *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1B ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
iregs       = iregs ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Scratchpad Register (SPR), write pattern	*/
/*so can read back & confirm SRP functional and thus board has UART		*/
oregs->cout_xon_1         = 0x55 ;  			/*SPR = 0x55 ????	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till SPR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1C ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 1B Function, mpa_srvc_reg_modem_1B		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1C FUNCTION, SET UP TO READ SPR BY ADDRESSING     *
 *					  MPA's AUX Reg		       	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1C  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*SPR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's SPR written to, Set up to Read SPR by address MPA's AUX reg		*/
oregs->cout_ctrl_sigs = 0x199 ;				/*Addr MPA's AUX reg	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/

/*Exit function & delay 4 frames thus allowing time for MPA to "see" AUX Reg 	*/
/*control signal setting							*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_reg_modem_1D ;
							/*Flow Cntrl Function	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of REG MODEM BOARD - 1C Function, mpa_srvc_reg_modem_1C	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1D FUNCTION, WRITE TO MPA's AUX REG INDICATING TO *
 *					  MPA TO READ AND (XMIT) UART's SPR REG*
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1D ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to MPA's AUX Register (SPR).  Write pattern indicating*/
/*to MPA to read UART's SPR.  Note: Change clean up function so if abort during	*/
/*read then outstanding read is cleaned up i.e. read character accounted for.	*/
slot->clean_up            = (void(*)(struct slot_struct      *, unsigned char))mpa_srvc_reg_modem_cln_rd ;
oregs->cout_xon_1         = 0x0F ;  			/*AUG REG byte = RD SPR	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/
slot->read_toggle         = 0x08 ;			/*Sv I/O Rd Toggle state*/

/*Set up & wait till AUX REG output sent					*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1E ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 1D Function, mpa_srvc_reg_modem_1D		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1E FUNCTION, WAIT FOR SPR BYTE TO BE RECIEVED &   *
 *					  VALIDATE BYTE			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1E  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP input area variables									   */
volatile struct cin_bnk_struct     	*actv_bank ;		/*Ptr to active bank		   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*AUX reg char xmit done*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*MPA's AUX Reg READ UART's SPR request sent, wait for SPR byte to be received	*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  */
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  */
if ( (actv_bank->bank_num_chars) != 1 )		/*Have recv'd SPR byte?	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk ( (DEADMAN_DELAY+5), gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_status = ERR_REG_MODEM_INPUT_TO ; 	/*Set cmpltn status	*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}
slot->clean_up            = (void(*)(struct slot_struct      *, unsigned char))mpa_srvc_reg_modem_clean_up ;
if ( (inc_tail (iregs)) != 0x00 )			/*Was tail updated OK?	*/
{							/*If no, error		*/
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_BAD_SSP ; 	/*Set cmpltn status	*/
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}
if ( *((unsigned char *)(&actv_bank->bank_fifo[0]) +		/*Rd OK?*/
               ((actv_bank->bank_fifo_lvl - 1) & 0x03) ) != 0x55 )	
{									/*If no	*/
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_NO_UART ; 	/*Set cmpltn status	*/
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Board appears to have a UART - set up to write different pattern into SPR	*/
oregs->cout_ctrl_sigs = 0x177 ;				/*Addr UART's SPR	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1F ;
							/*Flow Cntrl Function	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of REG MODEM BOARD - 1E Function, mpa_srvc_reg_modem_1E	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1F FUNCTION, ATTEMPT TO WRITE 2nd CHAR TO UART's  *
 *					  SPR SO CAN ATTEMPT TO READ BACK &    *
 *					  DETERMINE IF BOARD HAS UART	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1F ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Scratchpad Register (SPR), write pattern	*/
/*so can read back & confirm SRP functional and thus board has UART		*/
oregs->cout_xon_1         = 0xAA ;  			/*SPR = 0xAA ????	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till SPR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1G ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 1F Function, mpa_srvc_reg_modem_1F		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1G FUNCTION, SET UP TO READ SPR BY ADDRESSING     *
 *					  MPA's AUX Reg		       	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1G  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*SPR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's SPR written to, Set up to Read SPR by address MPA's AUX reg		*/
oregs->cout_ctrl_sigs = 0x199 ;				/*Addr MPA's AUX reg	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/

/*Exit function & delay 4 frames thus allowing time for MPA to "see" AUX Reg 	*/
/*control signal setting							*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_reg_modem_1H ;
							/*Flow Cntrl Function	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of REG MODEM BOARD - 1G Function, mpa_srvc_reg_modem_1G	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1H FUNCTION, WRITE TO MPA's AUX REG INDICATING TO *
 *					  MPA TO READ AND (XMIT) UART's SPR REG*
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1H ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to MPA's AUX Register (SPR).  Write pattern indicating*/
/*to MPA to read UART's SPR.  Note: Change clean up function so if abort during	*/
/*read then outstanding read is cleaned up i.e. read character accounted for.	*/
slot->clean_up            = (void(*)(struct slot_struct      *, unsigned char))mpa_srvc_reg_modem_cln_rd ;
oregs->cout_xon_1         = 0x07 ;  			/*AUG REG byte = RD SPR	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/
slot->read_toggle         = 0x00 ;			/*Sv I/O Rd Toggle state*/

/*Set up & wait till AUX REG output sent					*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_1I ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 1H Function, mpa_srvc_reg_modem_1H		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 1I FUNCTION, WAIT FOR SPR BYTE TO BE RECIEVED &   *
 *					  VALIDATE BYTE			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_1I  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP input area variables									   */
volatile struct cin_bnk_struct     	*actv_bank ;		/*Ptr to active bank	 	   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*AUX reg char xmit done*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*MPA's AUX Reg READ UART's SPR request sent, wait for SPR byte to be received	*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  */
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  */
if ( (actv_bank->bank_num_chars) != 1 )		/*Have recv'd SPR byte?	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk ( (DEADMAN_DELAY+5), gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_status = ERR_REG_MODEM_INPUT_TO ; 	/*Set cmpltn status	*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}
slot->clean_up            = (void(*)(struct slot_struct      *, unsigned char))mpa_srvc_reg_modem_clean_up ;
if ( (inc_tail (iregs)) != 0x00 )			/*Was tail updated OK?	*/
{							/*If no, error		*/
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_BAD_SSP ; 	/*Set cmpltn status	*/
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}
if ( *((unsigned char *)(&actv_bank->bank_fifo[0]) +		/*Rd OK?*/
               ((actv_bank->bank_fifo_lvl - 1) & 0x03) ) != 0xAA )	
{									/*If no	*/
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_NO_UART ; 	/*Set cmpltn status	*/
	slot->slot_state  = 0xFE ;			/*Set slot's state = 	*/
							/*unknown board		*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Board appears to have a UART - set up to address UART's IER			*/
oregs->cout_ctrl_sigs = 0x111 ;				/*Addr UART's IER	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_2 ;
							/*Flow Cntrl Function	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of REG MODEM BOARD - 1I Function, mpa_srvc_reg_modem_1I	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 2nd FUNCTION, INITIALIZE (WRITE TO) UART's IER    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_2  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Interrupt Enable Register (IER)		*/
oregs->cout_xon_1         = 0x08 ;			/*IER=Modem Stat int ena*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till IER output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 2nd Function, mpa_srvc_reg_modem_2		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 3rd FUNCTION, SET UP MPA SO OUTPUT GOES TO SLOT'S *
 *                                         AUX REG - MPA-to-UART FLOW CONTROL  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_3  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*IER Reg xmit cmplt	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Setting UART's IER register complete.  Set up so output sent to MPA's AUX reg	*/
/*MPA-to-UART Flow Control							*/
oregs->cout_ctrl_sigs = 0x1AA ;				/*Send control sigs to	*/
							/*MPA indicat'g to direct*/
							/*output to MPA-to-UART	*/
							/*Flow Cntrl		*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_4 ;		/*Disable MPA-to-UART	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 3rd FUNCTION, mpa_srvc_reg_modem_3		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 4th FUNCTION, INITIALIZE MPA-to-UART FLOW CONTROL *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_2,		       *
 *				 "mpa_srvc_mpa_flow_2" and 		       *
 *				 "mpa_srvc_mod_sets_2			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_4  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							     /*If no, modem pulled  */
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to MPA AUX register - MPA-to-UART Flow Control setting*/
oregs->cout_xon_1         = slot->flow_setting ;	/*AUX Reg flow ctrl	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till MPA AUX Reg Flow Control setting (character) sent		*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_5 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 4th FUNCTION, mpa_srvc_reg_modem_4		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 5th FUNCTION, ADDRESS UART's LCR or FCR REGISTER  *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_3" and		       *
 * 				 "mpa_srvc_mod_sets_3"			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_5  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*AUX Reg char xmit done*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*MPA-to-UART Flow Control established, set output control signals to address	*/
/*UART's FIFO Control Register (FCB) if Registering Modem or Line Control 	*/
/*Register (LCR) if some other Service using this function			*/
if ( caller_parm == 0 )				/*R15*/	/*Performing Reg Modem?	*/
{							/*If yes, init. FCB reg	*/
	oregs->cout_ctrl_sigs = 0x122 ;		/*R15*/	/*Addr UART's FCR	*/
	marb->cont_funct = mpa_srvc_reg_modem_5a ; /*R15*/
}							/*Function being called	*/
else							/* by some Service other*/
{							/*  then Reg Modem	*/
	oregs->cout_ctrl_sigs = 0x133 ;			/*Addr UART's LCR	*/
	marb->cont_funct = mpa_srvc_reg_modem_6 ;
}
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 5th Function, mpa_srvc_reg_modem_5		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 5a FUNCTION, INITIALIZE UART's FCR, FIFO Control  *
 *			     Register (if it exists)			       *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_hard_rst_6a"			       *
 *                                                                             *
 *******************************************************************************/
/*R15*/

static void  mpa_srvc_reg_modem_5a  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's FIFO Control Register (FCR)			*/
oregs->cout_xon_1 = 0x07 ;				/*Enable 16550 FIFO	*/
							/* (assumes 16550)	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till FCR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_5b ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 5a Function, mpa_srvc_reg_modem_5a		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  REGISTER MODEM BOARD - 5b FUNCTION, ADDRESS UART's LCR REGISTER 	       *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_hard_rst_8"			       *
 *                                                                             *
 *******************************************************************************/
/*R15*/

static void  mpa_srvc_reg_modem_5b  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*FCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's FCR, FIFO Control Register, written to (if it exists), set output 	*/
/*control signals to address UART's Line Control Register (LCR)			*/
oregs->cout_ctrl_sigs = 0x133 ;				/*Addr UART's LCR	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_6 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 5b Function, mpa_srvc_reg_modem_5b		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 6th FUNCTION, INITIALIZE UART's LCR REGISTER      *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_4" 		       *
 *				 "mpa_srvc_hard_rst_7" and		       *
 * 				 "mpa_srvc_mod_sets_4"			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_6  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Line Control Register (LCR)			*/
oregs->cout_xon_1         = 0x80   	      |		/*LCR=Divsr Latch ena +	*/
			    slot->num_bits    |		/*    char size +	*/
			    slot->stop_bits   |		/*    stop bits +	*/
			    slot->parity      |		/*    parity    +	*/
                            slot->break_state ;		/*    break state	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till LCR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_7 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 6th Function, mpa_srvc_reg_modem_6		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  REGISTER MODEM BOARD - 7th FUNCTION, ADDRESS UART's DIVISOR LATCH REGISTER *
 *			(Least Significant byte) DLL			       *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_5" and		       *
 *				 "mpa_srvc_hard_rst_8"			       *
 *				 "mpa_srvc_mod_sets_5"			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_7  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*LCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's LCR written to, set output control signals to address UART's Divisor 	*/
/*Latch, least significant byte (DLL)						*/
oregs->cout_ctrl_sigs = 0x100 ;				/*Addr UART's DLL	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_8 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 7th Function, mpa_srvc_reg_modem_7		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 8th FUNCTION, INITIALIZE UART's DLL REGISTER      *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_6",		       *
 *				 "mpa_srvc_hard_rst_9" and		       *
 *				 "mpa_srvc_mod_sets_6" 			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_8  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Divisor Latch least significant byte (DLL)	*/
oregs->cout_xon_1         = (unsigned char)(slot->baud & 0xFF) ;
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till DLL output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_9 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 8th Function, mpa_srvc_reg_modem_8		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  REGISTER MODEM BOARD - 9th FUNCTION, ADDRESS UART's DIVISOR LATCH REGISTER *
 *			(Most Significant byte) DLM			       *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_7",		       *
 *				 "mpa_srvc_hard_rst_10" and		       *
 *				 "mpa_srvc_mod_sets_7"			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_9  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*DLL char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's DLL written to, set output control signals to address UART's Divisor 	*/
/*Latch, most significant byte (DLM)						*/
oregs->cout_ctrl_sigs = 0x111 ;				/*Addr UART's DLL	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_10 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 9th Function, mpa_srvc_reg_modem_9		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 10th FUNCTION, INITIALIZE UART's DLM REGISTER     *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_8" 		       *
 *				 "mpa_srvc_hard_rst_11" and		       *
 *				 "mpa_srvc_mod_sets_8"			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_10 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Divisor Latch most significant byte (DLM)	*/
oregs->cout_xon_1         = (unsigned char)( (slot->baud >> 8) & 0xFF ) ;
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till DLM output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_11 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 10th Function, mpa_srvc_reg_modem_10		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 11th FUNCTION, ADDRESS UART's LCR REGISTER	       *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_9",		       *
 *				 "mpa_srvc_hard_rst_12",		       *
 * 				 "mpa_srvc_start_break_1"  		       *
 * 				 "mpa_srvc_stop_break_1" and		       *
 * 				 "mpa_srvc_mod_sets_9"			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_11  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*DLM char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's DLM written to, clear LCR's divisor latch enable bit			*/
oregs->cout_ctrl_sigs = 0x133 ;				/*Addr UART's LCR	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_12 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 11th Function, mpa_srvc_reg_modem_11		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 12th FUNCTION, CLEAR UART'S LCR REGISTER DIVISOR  *
 *                                          LATCH ENABLE BIT		       *
 *                                                                             *
 * NOTE: Function also called by "mpa_srvc_init_modem_10",		       *
 *				 "mpa_srvc_hard_rst_13", 		       *
 * 				 "mpa_srvc_break_2", and		       *
 *				 "mpa_srvc_mod_sets_10			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_12 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( ( (caller_parm == 0) && (slot->slot_state != 0x80) ) || /*Is Brd still present?*/
     ( (caller_parm != 0) && (slot->slot_state != 0xFF) ) )
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Line Control Register (LCR)			*/
oregs->cout_xon_1         = 0x00 	      | 	/*LCR=Divsr Latch dsa +	*/
			    slot->num_bits    |		/*    char size +	*/
			    slot->stop_bits   |		/*    stop bits +	*/
			    slot->parity      |		/*    parity    +	*/
                            slot->break_state ;		/*    break state	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till LCR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_13 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 12th Function, mpa_srvc_reg_modem_12		*/

/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 13th FUNCTION, ADDRESS UART's SPR REGISTER	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_13  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*LCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's LCR written to, set output control signals to address	UART's  	*/
/*Scratchpad Register (SPR)							*/
oregs->cout_ctrl_sigs = 0x177 ;				/*Addr UART's SPR	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_16 ;		/*NOTE: Functns 14 & 15	*/
							/*	deleted. 	*/
							/*	Originaly init'd*/
							/*	MCR		*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 13th Function, mpa_srvc_reg_modem_13		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 16th FUNCTION, INITIALIZE UART's SPR REGISTER     *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_16 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's Scratchpad Register (SPR)			*/
oregs->cout_xon_1         = 0x00 ;  			/*SPR = 0		*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till SPR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_17 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 16th Function, mpa_srvc_reg_modem_16		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 17th FUNCTION, RESET SLOT			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_17  ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*SPR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's SPR written to, Reset SLOT						*/
oregs->cout_ctrl_sigs = 0x188 ;				/*MPA's Slot Reset	*/
slot->read_toggle     = 0x00 ;				/*UART I/O Read toggle	*/
slot->frame_ctr  = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_18 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 17th Function, mpa_srvc_reg_modem_17		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 18th FUNCTION, SAVE MODEM/UART's BASE I/O ADDR &  *
 *					    BEGIN TO ESTABLISH SSP OUTPUT      *
 *					    CONTROL SIGNAL SETTINGS	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_18 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

unsigned short int	raw_reset_tdm ;			/*Reset TDM		*/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*8 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that Slot should be reset, get base I/O port address from TDM			*/
raw_reset_tdm  = iregs->cin_tdm_early ;			/*Get raw TDM		*/
slot->io_base  = (unsigned short int)(0x02E8 |		/*Develop I/O port addr	*/
		 ((((unsigned int)raw_reset_tdm) & 0x200) >> 1)   |
		 ((((unsigned int)raw_reset_tdm) & 0x100) >> 4) ) ;

/*Set up & establish inverse of Requestor's output control signals in order for	*/
/*MPA to "see" change in control signals which in turn causes MPA to write to	*/
/*UART's MCR. NOTE: "Toggling" control signals assures UART MCR control signals	*/
/*coherent with Requestor's SSP control signal settings				*/
oregs->cout_ctrl_sigs = ((slot->sv_cout_cntrl_sig & 0x0237) | 0x0004) ^ 0x0003 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_19 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 18th Function, mpa_srvc_reg_modem_18		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    REGISTER MODEM BOARD - 19th FUNCTION, SET SSP OUTPUT CONTROL SIGNAL HENCE*
 *					    SINCE CONTROL SIGNALS CHANGED, UART*
 *					    MCR IS UPDATED		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_19 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" inverse of actual control signals	*/
/*hence MCR register's control signals may have been refreshed (if MPA saved 	*/
/*control signals different).  Restore requestor's control signals - this 	*/
/*assures MPA updates MCR to reflect requestor's control signal setting		*/
oregs->cout_ctrl_sigs = (slot->sv_cout_cntrl_sig & 0x0237) | 0x0004 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_reg_modem_20 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of REG MODEM BOARD - 19th Function, mpa_srvc_reg_modem_19		*/

/*#page*/
/*******************************************************************************
 *                                                                             *
 *    	      REGISTER MODEM BOARD - 20th FUNCTION, REQUEST COMPLETE           *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_reg_modem_20 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0x80 )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_REG_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signals, complete request	*/
slot->slot_state = 0xFF ;				/*Indicate Modem presnt	*/
slot->init_state = 0xFF ;				/*MODEM/UART Openned	*/
mpa_srvc_reg_modem_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
marb->srvc_state  = 0x00 ;				/*Set Srvc State = Done	*/
return ;						/*Exit, Request Complete*/

}	/*End of REG MODEM BOARD - 20th Function, mpa_srvc_reg_modem_20		*/





/*#stitle SLOT SERVICE - REGISTER MODEM BOARD, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 * REGISTER MODEM BOARD, LOCAL FUNCTION - Clean Up SSP (mpa_srvc_reg_modem_clean_up)	*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_reg_modem_clean_up  (struct slot_struct         *slot,		*
 *					     unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_reg_modem_clean_up (struct slot_struct        	*slot,
					     unsigned char		type)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)			   */
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

oregs = slot->output ;					/*Slot's output regs ptr*/

/*Delay if actually canceling/aborting an outstanding Register Modem Board MARB	*/
/*that is in process of Restting Slot. This allows time for MPA to handle Reset	*/
if ((type == 0xFF) 					&&
    (slot->actv_marb->srvc_status == MARB_OUT_CANCELED) &&
    (oregs->cout_ctrl_sigs == 0x188)  			)
     /*If abort'g MARB, delay to give MPA chance to reset			*/
	while ( frame_delay_chk ( (unsigned short int)0x08, 	
				  slot->actv_marb->sspcb->global, slot)	== 0 )
	{ }

/*Restore saved SSP registers							*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x0237) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_reg_modem_clean_up	*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 * 	REGISTER MODEM BOARD, LOCAL FUNCTION - Outstanding Read Clean Up SSP 		*
 *				(mpa_srvc_reg_modem_cln_rd)				*
 *                                                                     			*
 * PURPOSE: To restore state of the SSP when UART Read outstanding has been aborted.	*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_reg_modem_cln_rd    (struct slot_struct         *slot,		*
 *					     unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_reg_modem_cln_rd   (struct slot_struct        	*slot,	   /*R9*/
					     unsigned char		type)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables									*/
volatile struct icp_gbl_struct      * gregs ;	/*Slot's Global regs	*/
volatile struct icp_in_struct      *	 iregs ;	/*Slot's Input regs	*/
volatile struct cin_bnk_struct      *	 actv_bank ;	/*Slot's active Bank	*/
volatile struct icp_out_struct      *	 oregs ;	/*Slot's Output regs	*/

/*Misc variables								*/
unsigned char				status ;	/*Returned status	*/



/************************
 *   START OF CODE	*
 ************************/

/*Set Slot's SSP pointer's							*/
gregs	  = slot->global ;				/*Slot's global regs ptr*/
iregs	  = slot->input ;				/*Slot's input regs ptr	*/
oregs	  = slot->output ;				/*Slot's output regs ptr*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  */
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  */

/*Assure MPA AUX reg character sent (Read UART toggle)				*/
while ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
        ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) ) /*AUX reg char xmit dne*/
{							 /*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0xFF )
		break ;					/*If deadman expired	*/
}

/*MPA's AUX Reg READ UART's SPR request sent, wait for SPR byte to be received	*/
while ( (actv_bank->bank_num_chars) != 1 )		/*Have recv'd SPR byte?	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk ( (DEADMAN_DELAY+5), gregs, slot) == 0xFF )
		break ;					/*If deadman expired	*/
}

/*May have read (and received) UART's char, if so update tail pointer to reflect*/
/*receipt of character								*/
if ( actv_bank->bank_num_chars == 1 )		/*Was UART char recv'd?	*/
	status = inc_tail ( iregs ) ;			/*If yes, update tail	*/

/*Have cleaned up outstanding read, now restore saved SSP registers by calling	*/
/*"standard" clean up routine.							*/
mpa_srvc_reg_modem_clean_up ( slot, type ) ;		/*Restore sv'd SSP regs	*/
return ;

}					/*End of mpa_srvc_reg_modem_cln_rd	*/







/*#stitle SLOT SERVICE - DEREGISTER MODEM BOARD (mpa_srvc_dereg_modem)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                  DEREGISTER MODEM BOARD - SERVICE ENTRY POINT               *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_dereg_modem (struct marb_struct       *marb)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_DEREG_MODEM_MARB) ;			/*If no, return error	*/
if ( (marb->in_use != 0) || (marb->req_type != 0x00) )	/*Init MARB Req & Waitd?*/
	return (ERR_DEREG_MODEM_MARB) ;			/*If no, return error	*/
marb->cont_funct = mpa_srvc_dereg_modem_1 ;		/*Init MARB's FN ptr	*/
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/

}			/*END of mpa_srvc_dereg_modem Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  DEREGISTER MODEM BOARD - 1st (and only) Function, Clean Up SLOTCB Function *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_dereg_modem_1 ( unsigned long		        caller_parm,
				      struct marb_struct      *  	marb,
	      			      struct ssp_struct      *   	sspcb,
	      			      struct slot_struct      *  	slot,
	      			      volatile struct icp_gbl_struct      * 	gregs,
				      volatile struct icp_in_struct      *	  	iregs,
	      			      volatile struct icp_out_struct      *	  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*Variables used to determine MPA Slot Power States				    */ 	/*R12*/

volatile struct icp_in_struct       *      mpa_1st_iregs;	/*MPA's 1st Slot Input regs 	   */
volatile struct cin_bnk_struct      *	actv_bank ;	/*Ptr to active bank	    	   */

struct slot_struct      *  	mpa_1st_slot ;		/*MPA's 1st SLOTCB	    */
unsigned char			slot_pwr_state ;	/*MPA SLOT PWR State	    */

/*Variables used to Deregister Modem Board					 */
unsigned short int		status ;		/*Status from Cancel MARB*/
struct marb_struct     	*	can_marb_ptr ;		/*Ptr to canceled MARB	 */
unsigned long int		svd_int_msk ;		/*Int Msk prior to Dsabl */

/*General working variables							*/	/*R12*/
unsigned int			i ;			/*Loop counter		*/
unsigned int			j ;			/*Loop counter		*/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
gregs       = gregs ;
iregs       = iregs ;
oregs       = oregs ;

/*For request's MPA, update the Power Switch state for each Slot Member	     			*/ 	/*R12*/
mpa_1st_iregs = sspcb->mpa[(marb->slot_chan >> 4)].input_base ;   /*Pt to MPA's base Input regs	*/
mpa_1st_slot  = sspcb->mpa[(marb->slot_chan >> 4)].slot_array_hd; /*Pt to 1st SLOT to update	*/
for ( i = 0; i < 16; i = i + 4 )				  /*Update MPA's SLOT PWR States*/
{
	actv_bank = ( ((mpa_1st_iregs+i)->cin_locks & 0x01) == 0x00 )		?
      		(volatile struct cin_bnk_struct      *)&((mpa_1st_iregs+i)->cin_bank_a) :
      		(volatile struct cin_bnk_struct      *)&((mpa_1st_iregs+i)->cin_bank_b) ;
	slot_pwr_state = ( ((actv_bank->bank_sigs) & 0x0800) == 0x0000)  ?
		(unsigned short int)0x0000 : (unsigned short int)0x00FF 	;
	for ( j = 0; j < 4; ++j )				  /*Loop to update SLOT PWR State*/
		(mpa_1st_slot+i+j)->power_state = slot_pwr_state ;
}													/*R12*/

/*Complete Request's validation							*/
if ( slot->slot_state == 0x00 )				/*Is Slot alrdy empty?	*/
{							/*If yes, error		*/
	marb->srvc_status = ERR_DEREG_MODEM_NOT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->req_outstnd != 0x00 ) 			/*Is a Req Outstand'g 	*/
{							/*If yes, go attempt to	*/
							/*cancel & clean up	*/
	status = mpa_srvc_can_marb (sspcb, 
				    marb->slot_chan, 
				    (struct marb_struct      *      *)&can_marb_ptr) ;
}

/*Prior to marking MPA Slot empty, and thus making it available for PnP 	*/
/*probing, gain access to SSPCB List thus Blocking Admin/Diag Poll function	*/
/*from potentually accessing a SLOTCB that is in process of being cleaned up	*/ /*R9*/
while ( (mpa_fn_access_sspcb_lst ((unsigned long int      *)&svd_int_msk)) != 0xFF ) {} /*Spin till access granted*/
slot->slot_state      = 0x00 ;				/*Indicate no Modem prsnt*/
slot->init_state      = 0x00 ;				/*MODEM/UART Closed	*/
slot->pnp_probe_state = 0x0000 ;			/*PnP not initialized	*/
marb->srvc_status     = 0x00 ;				/*Set completion status	*/
mpa_fn_relse_sspcb_lst (svd_int_msk) ;						    /*R9*/
return ;						/*Exit, Request Complete*/

}	/*End of DEREGISTER MODEM BOARD - 1st Function, mpa_srvc_dereg_modem_1	*/








/*#stitle SLOT SERVICE - SLOT STATUS (mpa_srvc_slot_status)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                  	SLOT STATUS - SERVICE ENTRY POINT                      *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_slot_status (struct marb_struct       *marb,
						  struct slot_struct      *      * slot_ret_parm)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/
unsigned char      *	sv_scratch_ptr ;		/*Requestor's scrtch ptr*/
unsigned short int	ret_status ;			/*Completion status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_SLOT_STATUS_MARB) ;			/*If no, return error	*/
if ( (marb->in_use != 0) || (marb->req_type != 0x00) )	/*Init MARB Req & Waitd?*/
	return (ERR_SLOT_STATUS_MARB) ;			/*If no, return error	*/
sv_scratch_ptr     = marb->scratch_area ;		/*Sv scratch ptr	*/
marb->scratch_area = (unsigned char      *)slot_ret_parm ; /*Pass ret parm ptr	*/
marb->cont_funct   = mpa_srvc_slot_status_1 ;		/*Init MARB's FN ptr	*/
ret_status         = async_req_hndlr(marb) ;		/*Go handle request	*/
marb->scratch_area = sv_scratch_ptr ;			/*Restore scratch ptr	*/
return (ret_status) ;					/*Request complete, exit*/
}			/*END of mpa_srvc_slot_status Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *	      SLOT STATUS - 1st (and only) Function, Return Slot pointer       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_slot_status_1  ( unsigned long		        caller_parm,
				       struct marb_struct      *  	marb,
	      			       struct ssp_struct      *   	sspcb,
	      			       struct slot_struct      *  	slot,
	      			       volatile struct icp_gbl_struct      * 	gregs,
				       volatile struct icp_in_struct      *	  	iregs,
	      			       volatile struct icp_out_struct      *  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
gregs       = gregs ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
*((struct slot_struct      *       *)marb->scratch_area) = slot ; /*Ret SLOTCB ptr*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Complete*/

}	/*End of SLOT STATUS - 1st Function, mpa_srvc_slot_status_1		*/









/*#stitle SLOT SERVICE - INITIALIZE (OPEN) UART/MODEM (mpa_srvc_init_modem)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *              INITIALIZE (OPEN) UART/MODEM - SERVICE ENTRY POINT             *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_init_modem (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_INIT_MODEM_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->gate = 0x00 ;				/*Set 1st call flag/gate*/
	marb->cont_funct = mpa_srvc_init_modem_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_INIT_MODEM ) /*Valid "Call Back"?*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_INIT_MODEM_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_reg_modem Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * INITIALIZE (OPEN) UART/MODEM BOARD - 1st FUNCTION, MPA-to-UART FLOW CONTROL *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_1 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned int		i ;				/*Loop counter		*/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( (marb->gate == 0x00) && (slot->req_outstnd != 0x00) ) /*Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_INIT_MODEM_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_INIT_MODEM_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, if 1st call initialize MARB's Slot Control Block	*/
if ( marb->gate == 0x00 )				/*1st time Fn called?	*/
{							/*If yes		*/
	slot->flow_setting = marb->sup_uchar0 ;		/*Save flow setting	*/
	slot->num_bits	   = marb->sup_uchar1 ;		/*Save # bits setting	*/
	slot->stop_bits	   = marb->sup_uchar2 ;		/*Save # stop bits	*/
	slot->parity   	   = marb->sup_uchar3 ;		/*Save parity setting	*/
	slot->baud	   = marb->sup_ushort ;		/*Save baud setting	*/

   /*Determine how to initialize SLOTCB's Open scratch area			*/
	if ( marb->scratch_area == (unsigned char      *)0 ) /*Is scratch area spec'd*/
		for ( i = 0; i < SLOT_SCRATCH_SZ; ++i )	     /*If no, 0 slot area    */
			slot->oscratch[i] = (unsigned char)0x00 ;
	else
		for ( i = 0; i < SLOT_SCRATCH_SZ; ++i )	     /*If yes, copy area*/
			slot->oscratch[i] = marb->scratch_area[i] ;

   /*Save SSP registers that will be used to initialized Slot & Modem Board's UART	*/
   /*and establish "clean up" function in case De-register Modem request handled 	*/
   /*asynchronously								*/
	slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
	slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
	slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
	slot->clean_up   = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_init_modem_clean_up ;
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->actv_srvc  = MARB_ACTV_SRVC_INIT_MODEM ;	/*Indicate outstnd'g req*/
	marb->gate = 0xFF ;				/*Set for repeat call	*/
	oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;
}

/*Determine if Slot already openned.  If not openned (i.e. Closed), set up and	*/
/*place MPA into Reset state thus flushing internal input/output buffers, else	*/
/*set up to set MPA-to-UART Flow Control					*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( slot->init_state != 0xFF )				/*Is Slot closed?	*/
{							/*If yes, init'g closed	*/
							/*slot, go issue reset	*/
	oregs->cout_ctrl_sigs = 0x188 ;			/*MPA's Slot Reset	*/
	slot->read_toggle     = 0x00 ;			/*UART I/O Read toggle	*/
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->cont_funct = mpa_srvc_init_modem_1A ;
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}

/*Set up so output sent to MPA's AUX reg - MPA-to-UART Flow Control		*/
oregs->cout_ctrl_sigs = 0x1AA ;				/*Send control sigs to	*/
							/*MPA indicat'g to direct*/
							/*output to AUX reg,MPA-*/
							/*to-UART Flow Cntrl	*/

/*Exit function & delay 4 frames thus allowing time for MPA to "see" AUX Reg 	*/
/*control signal setting							*/
marb->cont_funct = mpa_srvc_init_modem_2 ;
							/*Flow Cntrl Function	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of INIT MODEM BOARD - 1st Function, mpa_srvc_init_modem_1	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  	       INITIALIZE MODEM BOARD - 1A FUNCTION, RESET COMPLETE	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_1A ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_INIT_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*8 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that Slot should be reset, set up so output sent to MPA's AUX reg - 		*/
/*MPA-to-UART Flow Control							*/
oregs->cout_ctrl_sigs = 0x1AA ;				/*Send control sigs to	*/
							/*MPA indicat'g to direct*/
							/*output to AUX reg,MPA-*/
							/*to-UART Flow Cntrl	*/
marb->cont_funct = mpa_srvc_init_modem_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}		/*End of INIT MODEM BOARD - 1A Function, mpa_srvc_init_modem_1A	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * INITIALIZE MODEM BOARD - 2nd FUNCTION, INITIALIZE MPA-to-UART FLOW CONTROL  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_2 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_4 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                       marb,  sspcb, slot,		/*Go send requestor's 	*/
		       gregs, iregs, oregs ) ;		/*MPA-to-UART Flow set'g*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till MPA AUX Reg Flow Control setting (character) sent then 	*/
/*address LCR									*/
marb->cont_funct = mpa_srvc_init_modem_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 2nd FUNCTION, mpa_srvc_init_modem_2		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	INITIALIZE MODEM BOARD - 3rd FUNCTION, ADDRESS UART'S LCR REGISTER     *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_3 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_5 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize LCR							*/
marb->cont_funct = mpa_srvc_init_modem_4 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 3rd FUNCTION, mpa_srvc_init_modem_3		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    INITIALIZE MODEM BOARD - 4th FUNCTION, INITIALIZE UART'S LCR REGISTER    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_4 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_6 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                       marb,  sspcb, slot,		/*Go init LCR		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then address DLL				*/
marb->cont_funct = mpa_srvc_init_modem_5 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 4th FUNCTION, mpa_srvc_init_modem_4		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    INITIALIZE MODEM BOARD - 5th FUNCTION, ADDRESS UART's DIVISOR LATCH      *
 *			REGISTER (Least Significant byte) DLL		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_5 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_7 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr DLL reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize DLL							*/
marb->cont_funct = mpa_srvc_init_modem_6 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 5th FUNCTION, mpa_srvc_init_modem_5		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    INITIALIZE MODEM BOARD - 6th FUNCTION, INITIALIZE UART'S DLL REGISTER    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_6 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_8 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                       marb,  sspcb, slot,		/*Go init DLL		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till DLL Char sent then address DLM				*/
marb->cont_funct = mpa_srvc_init_modem_7 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 6th FUNCTION, mpa_srvc_init_modem_6		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    INITIALIZE MODEM BOARD - 7th FUNCTION, ADDRESS UART's DIVISOR LATCH      *
 *			REGISTER (Most Significant byte) DLM		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_7 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_9 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr DLM reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize DLM							*/
marb->cont_funct = mpa_srvc_init_modem_8 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 7th FUNCTION, mpa_srvc_init_modem_7		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    INITIALIZE MODEM BOARD - 8th FUNCTION, INITIALIZE UART'S DLM REGISTER    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_8 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_10 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                        marb,  sspcb, slot,		/*Go init DLM		*/
		        gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till DLM Char sent then address LCR (to clr divisor latch ena)	*/
marb->cont_funct = mpa_srvc_init_modem_9 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 8th FUNCTION, mpa_srvc_init_modem_8		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	INITIALIZE MODEM BOARD - 9th FUNCTION, ADDRESS UART'S LCR REGISTER     *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_9 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_11 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                        marb,  sspcb, slot,		/*Go set control sig's	*/
		        gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize LCR							*/
marb->cont_funct = mpa_srvc_init_modem_10 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 9th FUNCTION, mpa_srvc_init_modem_9		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  INITIALIZE MODEM BOARD - 10th FUNCTION, CLEAR UART'S LCR REGISTER DIVISOR  *
 *                                          LATCH ENABLE BIT		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_10 ( unsigned long		        caller_parm,
				      struct marb_struct      *  	marb,
	      			      struct ssp_struct      *   	sspcb,
	      			      struct slot_struct      *  	slot,
	      			      volatile struct icp_gbl_struct      * 	gregs,
				      volatile struct icp_in_struct      *	  	iregs,
	      			      volatile struct icp_out_struct      *	  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_12 ( (unsigned long)MARB_ACTV_SRVC_INIT_MODEM,
                        marb,  sspcb, slot,		/*Go init LCR		*/
		        gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then set slot's/chan's control signals for	*/
/*normal operation								*/
marb->cont_funct = mpa_srvc_init_modem_11 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 10th FUNCTION, mpa_srvc_init_modem_10	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  INITIALIZE MODEM BOARD - 11th FUNCTION, SET CONTROL SIGNALS FOR NORMAL USE *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_11 ( unsigned long		        caller_parm,
				      struct marb_struct      *  	marb,
	      			      struct ssp_struct      *   	sspcb,
	      			      struct slot_struct      *  	slot,
	      			      volatile struct icp_gbl_struct      * 	gregs,
				      volatile struct icp_in_struct      *	  	iregs,
	      			      volatile struct icp_out_struct      *	  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_INIT_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*LCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_INIT_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's LCR written to, determine if Slot already openned.  If not openned,	*/
/*set up & place MPA into Reset state thus flushing internal input/output	*/
/*buffers, else complete request by setting control signals			*/
if ( slot->init_state != 0xFF )				/*Is Slot closed?	*/
{							/*If yes, init'g closed	*/
							/*slot, go issue reset	*/
	oregs->cout_ctrl_sigs = 0x188 ;			/*MPA's Slot Reset	*/
	slot->read_toggle     = 0x00 ;			/*UART I/O Read toggle	*/
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->cont_funct = mpa_srvc_init_modem_12 ;	/*Pt to reset cmplt fn	*/
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}

/*Slot already openned (do not issue reset), set Slot's control signals for 	*/
/*normal operation								*/
oregs->cout_ctrl_sigs = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_init_modem_13 ;		/*Point to req cmplt fn	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}	/*End of INIT MODEM BOARD - 11th FUNCTION, mpa_srvc_init_modem_11	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  	       INITIALIZE MODEM BOARD - 12th FUNCTION, RESET COMPLETE	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_12 ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_INIT_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*8 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that Slot should be reset, set Slot's control signals for normal operation	*/
oregs->cout_ctrl_sigs = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_init_modem_13 ;		/*Point to req cmplt fn	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of INIT MODEM BOARD - 12th Function, mpa_srvc_reg_modem_12	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  	       INITIALIZE MODEM BOARD - 13th FUNCTION, REQUEST COMPLETE	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_init_modem_13 ( unsigned long		        caller_parm,
				      struct marb_struct      *  	marb,
	      			      struct ssp_struct      *   	sspcb,
	      			      struct slot_struct      *  	slot,
	      			      volatile struct icp_gbl_struct      * 	gregs,
				      volatile struct icp_in_struct      *	  	iregs,
	      			      volatile struct icp_out_struct      *	  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_INIT_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
{
	marb->srvc_state = 0xFF ;			/*Set "call back" state	*/
	return ;					/*4 frames not elapsed	*/
}

/*Have delayed long enough for MPA to "see" control signals, complete request	*/
slot->init_state = 0xFF ;				/*MODEM/UART Openned	*/
mpa_srvc_init_modem_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Complete*/

}	/*End of INIT MODEM BOARD - 13th FUNCTION, mpa_srvc_init_modem_13	*/








/*#stitle SLOT SERVICE - INITIALIZE MODEM BOARD, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 * INITIALIZE MODEM BOARD, LOCAL FUNCTION - Clean Up SSP (mpa_srvc_init_modem_clean_up)	*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_init_modem_clean_up  (struct slot_struct       * slot,		*
 *					      unsigned char		 type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_init_modem_clean_up (struct slot_struct      *  slot,
					      unsigned char		 type)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

oregs = slot->output ;					/*Slot's output regs ptr*/

/*Delay if actually canceling/aborting an outstanding Register Modem Board MARB	*/
/*that is in process of Restting Slot. This allows time for MPA to handle Reset	*/
if ((type == 0xFF) 					&&
    (slot->actv_marb->srvc_status == MARB_OUT_CANCELED) &&
    (oregs->cout_ctrl_sigs == 0x188)  			)
     /*If abort'g MARB, delay to give MPA chance to reset			*/
	while ( frame_delay_chk ( (unsigned short int)0x08, 	
				  slot->actv_marb->sspcb->global, slot)	== 0 )
	{ }

/*Restore saved SSP registers							*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_init_modem_clean_up	*/








/*#stitle SLOT SERVICE - MODIFY UART/MODEM SETTINGS (mpa_srvc_modify_settings)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *              MODIFY UART/MODEM SETTINGS - SERVICE ENTRY POINT               *
 *                                                                             *
 *******************************************************************************/
/*R6*/
extern unsigned short int	mpa_srvc_modify_settings (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_MOD_MODEM_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_mod_sets_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_MOD_SETS) /*Valid "Call Back"?	*/
	{						 /*If no, exit		*/
		marb->srvc_status = ERR_MOD_MODEM_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_modify_settings Service Entry function*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *     MODIFY UART/MODEM SETTINGS - 1st FUNCTION, CHECK IF ANY SETTINGS CHANGE *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_1  ( unsigned long			caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
				  )
{

/************************
 *     LOCAL DATA	*
 ************************/

unsigned int		i ;				/*Loop counter		*/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 ) 			/*Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_MOD_MODEM_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MOD_MODEM_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MOD_MODEM_NOPN ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, set up to modify UART's setting			*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->actv_srvc  = MARB_ACTV_SRVC_MOD_SETS ;		/*Indicate outstnd'g req*/

/*Save SSP registers that will be used to modify UART's settings		*/
slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
slot->clean_up   = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_mod_sets_clean_up ;
oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;

/*Check if should update caller's Open Scratch Area in SLOTCB			*/
if ( marb->scratch_area != (unsigned char      *)0 ) 	/*Is scratch area spec'd*/
	for ( i = 0; i < SLOT_SCRATCH_SZ; ++i )	     	/*If yes, copy area	*/
		slot->oscratch[i] = marb->scratch_area[i] ;

/*Set up and determine if Flow Control Settings are changing			*/
if ( marb->sup_uchar0 != slot->flow_setting )		/*Is Flow Cntrl chng'g?	*/
{							/*If yes		*/
	slot->flow_setting = marb->sup_uchar0 ;		/*Save new flow setting	*/
	oregs->cout_ctrl_sigs = 0x1AA ;			/*Send control sigs to	*/
							/*MPA indicat'g to direct*/
							/*output to AUX reg,MPA-*/
							/*to-UART Flow Cntrl	*/
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->cont_funct = mpa_srvc_mod_sets_2 ;	/*Flow Cntrl Function	*/
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}

/*Set up and determine if Baud Rate setting is changing				*/
if ( marb->sup_ushort != slot->baud )			/*Is Baud Rate chng'g?	*/
{							/*If yes		*/
	slot->baud      = marb->sup_ushort ;		/*Save new Baud Rate	*/
	slot->num_bits	= marb->sup_uchar1 ;		/*Assure LCR bits as per*/
	slot->stop_bits	= marb->sup_uchar2 ;		/* Request since LCR is	*/
	slot->parity   	= marb->sup_uchar3 ;		/*  Writtn to when chnge*/
							/*   Baud Rate		*/
	oregs->cout_ctrl_sigs = 0x133 ;			/*Addr UART's LCR	*/
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->cont_funct = mpa_srvc_mod_sets_4 ;
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}

/*Set up and determine if Line Control Register settings are changing		*/
if ( (marb->sup_uchar1 != slot->num_bits)  ||		/*Is char size chng'g?	*/
     (marb->sup_uchar2 != slot->stop_bits) ||		/*Is # stop bits chng'g?*/
     (marb->sup_uchar3 != slot->parity)	   )		/*Is parity changing?	*/
{							/*If yes		*/
	slot->num_bits	= marb->sup_uchar1 ;		/*Save # bits setting	*/
	slot->stop_bits	= marb->sup_uchar2 ;		/*Save # stop bits	*/
	slot->parity   	= marb->sup_uchar3 ;		/*Save parity setting	*/
	oregs->cout_ctrl_sigs = 0x133 ;			/*Addr UART's LCR	*/
	slot->frame_ctr       = gregs->gicp_frame_ctr;	/*Snap shot for timing	*/
	marb->cont_funct = mpa_srvc_mod_sets_10 ;
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}

/*No Modem Settings are being changed, return to caller with successful		*/
/*completion status								*/
mpa_srvc_mod_sets_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_state  = 0x00 ;				/*Set Srvc State = Done	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
marb->actv_srvc   = 0x00 ;				/*Set no req outstndg	*/
return ;						/*Exit, Request Complete*/

}		/*End of MODIFY UART/MODEM SETTINGS - 1st Fn, mpa_srvc_mod_sets_1*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 2nd FUNCTION, INIT MPA-to-UART FLOW CONTROL    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_2 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_4 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                       marb,  sspcb, slot,		/*Go send requestor's 	*/
		       gregs, iregs, oregs ) ;		/*MPA-to-UART Flow set'g*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till MPA AUX Reg Flow Control setting (character) sent then 	*/
/*address LCR									*/
marb->cont_funct = mpa_srvc_mod_sets_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 2nd FUNCTION, mpa_srvc_mod_sets_2	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 3rd FUNCTION, CHK FOR BAUD RATE or LCR CHANGES *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_3 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_5 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*MPA-to-UART Flow Control updated, set up & determine if Baud Rate and/or LCR	*/
/*(char size, parity, stop bits) is changing					*/
if ( marb->sup_ushort != slot->baud )			/*Is Baud Rate chng'g?	*/
{							/*If yes		*/
	slot->baud      = marb->sup_ushort ;		/*Save new Baud Rate	*/
	slot->num_bits	= marb->sup_uchar1 ;		/*Assure LCR bits as per*/
	slot->stop_bits	= marb->sup_uchar2 ;		/* Request since LCR is	*/
	slot->parity   	= marb->sup_uchar3 ;		/*  Writtn to when chnge*/
							/*   Baud Rate		*/
	marb->cont_funct = mpa_srvc_mod_sets_4 ;	/*Set up to wrt to LCR	*/
							/*so can change Divisors*/
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}
if ( (marb->sup_uchar1 != slot->num_bits)  ||		/*Is char size chng'g?	*/
     (marb->sup_uchar2 != slot->stop_bits) ||		/*Is # stop bits chng'g?*/
     (marb->sup_uchar3 != slot->parity)	   )		/*Is parity changing?	*/
{							/*If yes		*/
	slot->num_bits	 = marb->sup_uchar1 ;		/*Save # bits setting	*/
	slot->stop_bits	 = marb->sup_uchar2 ;		/*Save # stop bits	*/
	slot->parity   	 = marb->sup_uchar3 ;		/*Save parity setting	*/
	marb->cont_funct = mpa_srvc_mod_sets_10 ;	/*Set up to wrt to LCR	*/
	return ;					/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}

/*Neither Baud Rate or LCR (char size, parity, stop bits) is being changed, set	*/
/*up to complete service/function by restoring Control Signals			*/
oregs->cout_ctrl_sigs = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_mod_sets_12 ;		/*Point to req cmplt fn	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 3rd FUNCTION, mpa_srvc_mod_sets_3	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 4th FUNCTION, INITIALIZE UART'S LCR REGISTER SO*
 *					      CAN CHANGE BAUD RATE DIVISORS    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_4 ( unsigned long			caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
			         )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_6 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                       marb,  sspcb, slot,		/*Go init LCR		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then address DLL				*/
marb->cont_funct = mpa_srvc_mod_sets_5 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 4th FUNCTION, mpa_srvc_mod_sets_4	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 5th FUNCTION, ADDRESS UART's DIVISOR LATCH     *
 *			                      REG (Least Significant byte) DLL *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_5 ( unsigned long			caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_7 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr DLL reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize DLL							*/
marb->cont_funct = mpa_srvc_mod_sets_6 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 5th FUNCTION, mpa_srvc_mod_sets_5	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 6th FUNCTION, INITIALIZE UART'S DLL REGISTER   *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_6 ( unsigned long			caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_8 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                       marb,  sspcb, slot,		/*Go init DLL		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till DLL Char sent then address DLM				*/
marb->cont_funct = mpa_srvc_mod_sets_7 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 6th FUNCTION, mpa_srvc_mod_sets_6	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 7th FUNCTION, ADDRESS UART's DIVISOR LATCH     *
 *			                      REG (Most Significant byte) DLM  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_7 ( unsigned long			caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_9 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr DLM reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize DLM							*/
marb->cont_funct = mpa_srvc_mod_sets_8 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 7th FUNCTION, mpa_srvc_mod_sets_7 */
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 8th FUNCTION, INITIALIZE UART'S DLM REGISTER   *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_8 ( unsigned long			caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_10 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                        marb,  sspcb, slot,		/*Go init DLM		*/
		        gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till DLM Char sent then address LCR (to clr divisor latch ena)	*/
marb->cont_funct = mpa_srvc_mod_sets_9 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 8th FUNCTION, mpa_srvc_mod_sets_8 */
/*#page*/
/*******************************************************************************
 *                                                                             *
 * MODIFY UART/MODEM SETTINGS - 9th FUNCTION, ADDRESS UART'S LCR REGISTER      *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_9 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_11 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                        marb,  sspcb, slot,		/*Go set control sig's	*/
		        gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize LCR							*/
marb->cont_funct = mpa_srvc_mod_sets_10 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MODIFY UART/MODEM SETTINGS - 9th FUNCTION, mpa_srvc_mod_sets_9	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *MODIFY UART/MODEM SETTINGS - 10th FUNCTION, CLEAR UART'S LCR REGISTER DIVISOR*
 *                                            LATCH ENABLE BIT		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_10 ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
				  )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_12 ( (unsigned long)MARB_ACTV_SRVC_MOD_SETS,
                        marb,  sspcb, slot,		/*Go init LCR		*/
		        gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then set slot's/chan's control signals for	*/
/*normal operation								*/
marb->cont_funct = mpa_srvc_mod_sets_11 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}   /*End of MODIFY UART/MODEM SETTINGS - 10th FUNCTION, mpa_srvc_mod_sets_10	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *MODIFY UART/MODEM SETTINGS - 11th FUNCTION, SET CONTROL SIGNALS FOR NORMAL   *
 *					      OPERATION			       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_11 ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
			          )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_MOD_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*LCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	mpa_srvc_mod_sets_clean_up (slot, (unsigned char)0xFF ) ;
	marb->srvc_status = ERR_MOD_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's LCR written to, Restore Contol Signals for normal use			*/
oregs->cout_ctrl_sigs = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_mod_sets_12 ;		/*Point to req cmplt fn	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}   /*End of MODIFY UART/MODEM SETTINGS - 11th FUNCTION, mpa_srvc_mod_sets_11	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *           MODIFY UART/MODEM SETTINGS - 12th FUNCTION, REQUEST COMPLETE      *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mod_sets_12 ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
			          )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_MOD_MODEM_REMOVED ;	/*Set completion status	*/
	return ;
}
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
{
	marb->srvc_state = 0xFF ;			/*Set "call back" state	*/
	return ;					/*4 frames not elapsed	*/
}

/*Have delayed long enough for MPA to "see" control signals, complete request	*/
mpa_srvc_mod_sets_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Complete*/

}   /*End of MODIFY UART/MODEM SETTINGS - 12th FUNCTION, mpa_srvc_mod_sets_12	*/








/*#stitle SLOT SERVICE - MODIFY UART/MODEM SETTINGS, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *MODIFY UART/MODEM SETTINGS, LOCAL FUNCTION - Clean Up SSP (mpa_srvc_mod_sets_clean_up)*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_mod_sets_clean_up  (struct slot_struct       *	slot,		*
 *					    unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_mod_sets_clean_up (struct slot_struct      *  slot,
					    unsigned char	       type)
{

/************************
 *     LOCAL DATA	*
 ************************/

volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
type = type ;

/*Restore saved SSP registers							*/
oregs = slot->output ;					/*Slot's output regs ptr*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_mod_sets_clean_up	*/






/*#stitle SLOT SERVICE - SET INTERNAL LOOP BACK (mpa_srvc_set_loop_back)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                  SET INTERNAL LOOP BACK - SERVICE ENTRY POINT               *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_set_loop_back (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_SET_LOOP_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_set_loop_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if (marb->actv_srvc != MARB_ACTV_SRVC_SET_LOOP) /*Valid "Call Back"? 	*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_SET_LOOP_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/

}			/*END of mpa_srvc_set_loop_back Service Entry function	*/

/*#page*/
/*******************************************************************************
 *                                                                             *
 *	      	SET INTERNAL LOOP BACK - 1st Function, Set Loop Back Control   *
 *						      Signal bits	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_set_loop_1 ( unsigned long		   caller_parm,
			           struct marb_struct      *  	   marb,
	      		           struct ssp_struct      *   	   sspcb,
	      		           struct slot_struct      *  	   slot,
	      		           volatile struct icp_gbl_struct      * gregs,
			           volatile struct icp_in_struct      *	   iregs,
	      		           volatile struct icp_out_struct      *  	   oregs
			         )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_SET_LOOP_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_SET_LOOP_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, set Internal Loop Back Control Signal bits		*/
oregs->cout_ctrl_sigs = (oregs->cout_ctrl_sigs & 0x02BF) | 0x008C; /*Set loop bk*/

/*Exit function & delay 0x1800 frames thus allowing time for MPA to "see" Internal*/
/*Loop Back control signal setting and allow time for non-UART based Modems to	*/
/*get set up (long delay required by Boca Modem)				*/
slot->frame_ctr  = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
slot->clean_up   = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_set_loop_clean_up ;
marb->actv_srvc  = MARB_ACTV_SRVC_SET_LOOP ;		/*Indicate outstnd'g req*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_set_loop_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}	/*End of Set Internal Loop Back  - 1st Function, mpa_srvc_set_loop_1	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *	      	SET INTERNAL LOOP BACK - 2nd Function, Delay		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_set_loop_2 ( unsigned long		   caller_parm,
				   struct marb_struct      *  	   marb,
	      			   struct ssp_struct      *   	   sspcb,
	      			   struct slot_struct      *  	   slot,
	      			   volatile struct icp_gbl_struct      * gregs,
				   volatile struct icp_in_struct      *	   iregs,
	      			   volatile struct icp_out_struct      *	   oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
oregs       = oregs ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_SET_LOOP_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x1800, gregs, slot ) == 0 )
	return ;					/*1800 frames not elapsed*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*to set MCR's Internal Loop Back bit, complete request				*/
slot->loop_back = 0xFF ;				/*Loop Back state = ON	*/
mpa_srvc_set_loop_clean_up (slot, (unsigned char)0x00 ); /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
marb->srvc_state  = 0x00 ;				/*Set completion state	*/
return ;						/*Exit, Request Cmplt	*/
}	/*End of Set Internal Loop Back  - 2nd Function, mpa_srvc_set_loop_back_2*/









/*#stitle SLOT SERVICE - SET INTERNAL LOOP BACK, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *         SET INTERNAL LOOP BACK - Clean Up SSP (mpa_srvc_set_loop_clean_up)		*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been CANCELED		*
 *											*
 * NOTES: 										*
 *                                                                     			*
 * CALL: void  mpa_srvc_set_loop_clean_up (struct slot_struct      *    slot,		*
 *			     	           unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal					*
 *					o 0xFF = Abnormal, aborted due to some failure	*
 *						 or Board removed			*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_set_loop_clean_up (struct slot_struct      *    slot,
					    unsigned char		 type)

{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clear Internal Loop Back Control Signals if abnormal "clean up"		*/
if (type != 0x00)					/*Abnormal call?	*/
{							/*If yes, clear loop back*/
	slot->output->cout_ctrl_sigs = (slot->output->cout_ctrl_sigs & 0x0237) | 0x0004 ;
}
slot->clean_up = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_set_loop_clean_up	*/









/*#stitle SLOT SERVICE - CLEAR INTERNAL LOOP BACK (mpa_srvc_clr_loop_back)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                  CLEAR INTERNAL LOOP BACK - SERVICE ENTRY POINT             *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_clr_loop_back (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_CLR_LOOP_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_clr_loop_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if (marb->actv_srvc != MARB_ACTV_SRVC_CLR_LOOP) /*Valid "Call Back"? 	*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_CLR_LOOP_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/

}			/*END of mpa_srvc_clr_loop_back Service Entry function	*/

/*#page*/
/*******************************************************************************
 *                                                                             *
 *	   CLEAR INTERNAL LOOP BACK - 1st Function, Clear Loop Back Control    *
 *						      Signal bits	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_clr_loop_1 ( unsigned long		   caller_parm,
			           struct marb_struct      *  	   marb,
	      		           struct ssp_struct      *   	   sspcb,
	      		           struct slot_struct      *  	   slot,
	      		           volatile struct icp_gbl_struct      * gregs,
			           volatile struct icp_in_struct      *	   iregs,
	      		           volatile struct icp_out_struct      *  	   oregs
			         )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_CLR_LOOP_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_CLR_LOOP_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, clear Internal Loop Back Control Signal bits		*/
oregs->cout_ctrl_sigs = (oregs->cout_ctrl_sigs & 0x0237) | 0x0004; /*Clr loop bk*/

/*Exit function & delay 256 frames thus allowing time for MPA to "see" Internal */
/*Loop Back control signal setting and allow time for non-UART based Modems to	*/
/*get set up (long delay required by Boca Modem)				*/
slot->frame_ctr  = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
slot->clean_up   = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_clr_loop_clean_up ;
marb->actv_srvc  = MARB_ACTV_SRVC_CLR_LOOP ;		/*Indicate outstnd'g req*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_clr_loop_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/
}	/*End of Clear Internal Loop Back  - 1st Function, mpa_srvc_clr_loop_1	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *	      	CLEAR INTERNAL LOOP BACK - 2nd Function, Delay		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_clr_loop_2 ( unsigned long		   caller_parm,
				   struct marb_struct      *  	   marb,
	      			   struct ssp_struct      *   	   sspcb,
	      			   struct slot_struct      *  	   slot,
	      			   volatile struct icp_gbl_struct      * gregs,
				   volatile struct icp_in_struct      *	   iregs,
	      			   volatile struct icp_out_struct      *	   oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
oregs       = oregs ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_CLR_LOOP_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x1800, gregs, slot ) == 0 )
	return ;					/*1800 frames not elapsed*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*to clear MCR's Internal Loop Back bit, complete request			*/
slot->loop_back = 0x00 ;				/*Loop Back state = OFF	*/
mpa_srvc_clr_loop_clean_up (slot, (unsigned char)0x00 ); /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
marb->srvc_state  = 0x00 ;				/*Set completion state	*/
return ;						/*Exit, Request Cmplt	*/
} /*End of Clear Internal Loop Back  - 2nd Function, mpa_srvc_clr_loop_back_2	*/









/*#stitle EXCEPTION SERVICE - CLEAR INTERNAL LOOP BACK, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *         CLEAR INTERNAL LOOP BACK - Clean Up SSP (mpa_srvc_clr_loop_clean_up)		*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been CANCELED		*
 *											*
 * NOTES: 										*
 *                                                                     			*
 * CALL: void  mpa_srvc_clr_loop_clean_up (struct slot_struct      *    slot,		*
 *			     	           unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal					*
 *					o 0xFF = Abnormal, aborted due to some failure	*
 *						 or Board removed			*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_clr_loop_clean_up (struct slot_struct      *    slot,
					    unsigned char		 type)
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
type = type ;

/*Start of actual "clean up"							*/
slot->clean_up = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_clr_loop_clean_up	*/








/*#stitle SLOT SERVICE - CLOSE MODEM BOARD (mpa_srvc_close_modem)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                       CLOSE MODEM BOARD - SERVICE ENTRY POINT               *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_close_modem (struct marb_struct      *  marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_CLOSE_MODEM_MARB) ;			/*If no, return error	*/
if ( (marb->in_use != 0) || (marb->req_type != 0x00) )	/*Init MARB Req & Waitd?*/
	return (ERR_CLOSE_MODEM_MARB) ;			/*If no, return error	*/
marb->cont_funct = mpa_srvc_close_modem_1 ;		/*Init MARB's FN ptr	*/
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/

}			/*END of mpa_srvc_close_modem Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *      CLOSE MODEM BOARD - 1st (and only) Function, Clean Up SLOTCB Function  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_close_modem_1 ( unsigned long		        caller_parm,
				      struct marb_struct      *  	marb,
	      			      struct ssp_struct      *   	sspcb,
	      			      struct slot_struct      *  	slot,
	      			      volatile struct icp_gbl_struct      * 	gregs,
				      volatile struct icp_in_struct      *	  	iregs,
	      			      volatile struct icp_out_struct      *	  	oregs
				    )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
gregs       = gregs ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_CLOSE_MODEM_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Slot occupied?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_CLOSE_MODEM_NOT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_CLOSE_MODEM_NOPN ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
slot->init_state = 0x00 ;				/*MODEM/UART Closed	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
oregs->cout_ctrl_sigs = (oregs->cout_ctrl_sigs & 0x0237) | 0x0004; /*Kill loop bk*/
return ;						/*Exit, Request Complete*/

}	/*End of CLOSE MODEM BOARD - 1st Function, mpa_srvc_close_modem_1	*/









/*#stitle SLOT SERVICE - CANCEL SLOT'S OUTSTANDING MARB (mpa_srvc_can_marb)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *              CANCEL SLOT'S OUTSTANDING MARB - SERVICE ENTRY POINT           *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_can_marb (struct ssp_struct       *        sspcb,
                               	  	       unsigned char                    slot_chan,
					       struct marb_struct      *      * ret_can_marb)
{

/************************
 *     LOCAL DATA	*
 ************************/

/*MPACB (MPA Control Block) and SLOTCB (Slot Control Block) variables		*/
unsigned int		 	mpa_index ;		/*MARB's MPA/Eng index	*/
struct slot_struct       	*slot ;			/*MARB's SLOT Cntrl Blk	*/

/*Variables used to cancel MARB							*/
struct marb_struct       	*outstnd_marb ;		/*Slot's Outstand'g MARB*/



/************************
 *   START OF CODE	*
 ************************/

*ret_can_marb = (struct marb_struct      *)0xFFFFFFFF ;	/*Ret SLOTCB ptr	*/
if ( val_ssp (sspcb) != 0x00 )				/*Is caller's parm OK?	*/
	return (ERR_CAN_SSPCB) ;			/*If no, exit		*/
mpa_index = slot_chan >> 4 ;				/*Index to MPA/Eng	*/
if ( sspcb->mpa[mpa_index].base_chan != 		/*Is caller's Chan OK?	*/
    ((mpa_index - (sspcb->mpa[mpa_index].eng_num)) << 4) ) 
	return (ERR_CAN_MPACB) ;			/*If no, error		*/
slot = (struct slot_struct       *)&((sspcb->mpa[mpa_index].eng_head)[(slot_chan & 0x0f)]) ;

if ( (slot->in_use     != 0xFF)		||		/*Is Caller's Slot valid*/
     (slot->alloc      != 0xFF)		||
     (slot->chan_num   != slot_chan)	||
     (slot->slot_state == 0x00)		||
     (slot->signature->in_use != 0xFF)  )
	return (ERR_CAN_SLOTCB) ;				/*If no, error		*/

/*Caller's request valid, set up and determine if Slot has outstanding MARB	*/
*ret_can_marb = (struct marb_struct      *)0x00000000 ;	/*Indicate request valid*/
if ( slot->req_outstnd != 0 )				/*Is MARB oustnd'g?	*/
{							/*If yes		*/
	outstnd_marb = slot->actv_marb ;		/*Get ptr to Slot's MARB*/
	if ( (outstnd_marb != (struct marb_struct      *)0)   && /*Is Slot's 	*/
	     (outstnd_marb->in_use      != 0) 		      && /*oustand MARB	*/
	     (outstnd_marb->actv_srvc   == slot->req_outstnd) && /*pointer OK?	*/
	     (outstnd_marb->srvc_status == 0x00) 	      &&
	     (outstnd_marb->srvc_state  == 0xFF) 	      &&
	     (outstnd_marb->signature   == outstnd_marb)      &&
	     (outstnd_marb->cont_funct  == slot->cont_funct)  )
	{						        /*If yes	*/
		outstnd_marb->in_use     = 0x00 ; 	/*Indicate avail MARB	*/
		outstnd_marb->actv_srvc  = 0x00 ; 	/*Set no req outstndg	*/
		outstnd_marb->srvc_status = MARB_OUT_CANCELED ;
		outstnd_marb->srvc_state  = 0x00 ;
		outstnd_marb->signature  = (struct marb_struct      *)0 ;
		outstnd_marb->cont_funct = (void (*)(unsigned long,
					  	     struct marb_struct       *,
	      			     	  	     struct ssp_struct        *,
	      			          	     struct slot_struct       *,
	      			          	     volatile struct mpa_global_s      *,
				          	     volatile struct mpa_input_s       *,
	      			          	     volatile struct mpa_output_s      *))0 ;
		*ret_can_marb = outstnd_marb ;
	    /*Call outstanding MARB Service Clean Up function			*/
		if ( slot->clean_up != (void (*)(struct slot_struct      *, unsigned char))0)
			(*slot->clean_up)(slot, (unsigned char)0xFF); /*If yes	*/
	}
}
slot->req_outstnd = 0x00 ;				/*Slot has no REQ Outstd*/
slot->actv_marb   = (struct marb_struct      *)0 ; 	/*No actv MARB		*/
slot->cont_funct  = (void (*)(unsigned long,		/*No "call back"	*/
		  	      struct marb_struct       *,
	      		      struct ssp_struct        *,
	      		      struct slot_struct       *,
	      		      volatile struct mpa_global_s      *,
			      volatile struct mpa_input_s       *,
	      		      volatile struct mpa_output_s      *))0 ;
slot->clean_up = (void (*)(struct slot_struct      *, unsigned char))0 ;
return (0x00) ;						/*Exit, Request Complete*/

}	/*End of CANCEL SLOT'S OUTSTANDING MARB - mpa_srvc_can_marb		*/









/*#stitle SLOT SERVICE - FLUSH MPA SLOT INPUT/OUTPUT BUFFERS (mpa_srvc_flush)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *          FLUSH MPA SLOT INPUT/OUTPUT BUFFERS - SERVICE ENTRY POINT          *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int    mpa_srvc_flush (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_MPA_FLUSH_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_flush_1 ;		/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_FLUSH ) 	/*Valid "Call Back"?  	*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_MPA_FLUSH_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_flush Service Entry function		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *     FLUSH MPA SLOT INPUT/OUTPUT BUFFERS - 1st FUNCTION, ISSUE SLOT RESET    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_flush_1     ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
				  )
{

/************************
 *     LOCAL DATA	*
 ************************/





/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_MPA_FLUSH_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MPA_FLUSH_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MPA_FLUSH_NOPN ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, save SSP registers that will be used to Flush Slot's	*/
/*Input/Output buffers								*/
slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs;
slot->clean_up            = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_flush_clean_up ;

/*Set slot's/channel's control signals to indicate to MPA to Flush (reset)	*/
/*Input/Output buffers								*/
oregs->cout_ctrl_sigs = 0x188 ;				/*MPA's Slot Reset	*/
slot->read_toggle     = 0x00 ;				/*UART I/O Read toggle	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/

/*Exit function & delay 8 frames thus allowing time for MPA to "see" RESET	*/
marb->actv_srvc  = MARB_ACTV_SRVC_FLUSH ;		/*Indicate outstnd'g req*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_flush_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Flush MPA Input/Output buffers - 1st function, mpa_srvc_flush_1*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *     FLUSH MPA SLOT INPUT/OUTPUT BUFFERS - 2nd FUNCTION, Address UART's FCR  *
 *					     FIFO Control Register	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_flush_2   ( unsigned long		        	caller_parm,
				  struct marb_struct      *  		marb,
	      			  struct ssp_struct      *   		sspcb,
	      			  struct slot_struct      *  		slot,
	      			  volatile struct icp_gbl_struct      * 	gregs,
				  volatile struct icp_in_struct      *	  	iregs,
	      			  volatile struct icp_out_struct      *	  	oregs
				)
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_MPA_FLUSH_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" RESET, point to UART's FIFO Control	*/
/*Register (FCR) assuming that it exists					*/
oregs->cout_ctrl_sigs = 0x122 ;			/*R15*/	/*Addr UART's FCR	*/
marb->cont_funct = mpa_srvc_flush_3 ; 		/*R15*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Flush MPA Input/Output buffers - 2nd function, mpa_srvc_flush_2*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *     FLUSH MPA SLOT INPUT/OUTPUT BUFFERS - 3rd FUNCTION, Flush UART's FIFO,  *
 *					     assuming it exists		       *
 *                                                                             *
 *******************************************************************************/
/*R15*/

static void  mpa_srvc_flush_3   ( unsigned long		        	caller_parm,
				  struct marb_struct      *  		marb,
	      			  struct ssp_struct      *   		sspcb,
	      			  struct slot_struct      *  		slot,
	      			  volatile struct icp_gbl_struct      * 	gregs,
				  volatile struct icp_in_struct      *	  	iregs,
	      			  volatile struct icp_out_struct      *	  	oregs
				)
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_MPA_FLUSH_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to UART's FIFO Control Register (FCR)			*/
oregs->cout_xon_1 = 0x07 ;				/*Enable 16550 FIFO	*/
							/* (assumes 16550)	*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till FCR output sent						*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_flush_4 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Flush MPA Input/Output buffers - 3rd function, mpa_srvc_flush_3*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *     FLUSH MPA SLOT INPUT/OUTPUT BUFFERS - 4th FUNCTION, Request Complete    *
 *                                                                             *
 *******************************************************************************/
/*R15*/

static void  mpa_srvc_flush_4   ( unsigned long		        	caller_parm,
				  struct marb_struct      *  		marb,
	      			  struct ssp_struct      *   		sspcb,
	      			  struct slot_struct      *  		slot,
	      			  volatile struct icp_gbl_struct      * 	gregs,
				  volatile struct icp_in_struct      *	  	iregs,
	      			  volatile struct icp_out_struct      *	  	oregs
				)
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_MPA_FLUSH_REMOVED ;	/*Set completion status	*/
	return ;
}
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*FCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		marb->srvc_state = 0xFF ;		/*Set "call back" state	*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_REG_MODEM_DEADMAN ; 	/*Set cmpltn status	*/
	return ;
}

/*Modem's FCR, FIFO Control Register, written to (if it exists) and hence UART	*/
/*FIFO flushed, Complete Request						*/
mpa_srvc_flush_clean_up  (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Cmplt	*/

}	/*END of Flush MPA Input/Output buffers - 4th function, mpa_srvc_flush_4*/









/*#stitle SLOT SERVICE - FLUSH MPA SLOT INPUT/OUTPUT BUFFERS, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *     FLUSH MPA SLOT INPUT/OUTPUT BUFFERS - Clean Up SSP (mpa_srvc_flush_clean_up)	*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion)					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_flush_clean_up  (struct slot_struct      *    slot,		*
 *			     	         unsigned char		      type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_flush_clean_up (struct slot_struct      *    slot,
					 unsigned char		      type)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

/*Delay if actually canceling/aborting an outstanding Flush MARB, thus allowing	*/
/*for MPA to handle Reset							*/
if ( (type == 0xFF) && (slot->actv_marb->srvc_status == MARB_OUT_CANCELED) )
     /*If abort'g MARB, delay to give MPA chance to reset			*/
	while ( frame_delay_chk ( (unsigned short int)0x08, 	
				  slot->actv_marb->sspcb->global, slot)	== 0 )
	{ }

/*Restore saved SSP registers							*/
oregs = slot->output ;					/*Slot's output regs ptr*/
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_flush_clean_up	*/









/*#stitle SLOT SERVICE - HARD RESET MODEM BOARD (mpa_srvc_hard_reset)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *          		HARD RESET MODEM BOARD - SERVICE ENTRY POINT           *
 *                                                                             *
 *******************************************************************************/
/*R5*/
extern unsigned short int   mpa_srvc_hard_reset (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_HRD_RST_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_hard_rst_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_HARD_RESET ) /*Valid "Call Back"?*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_HRD_RST_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_hard_reset Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 1st FUNCTION, ISSUE HARD MODEM BOARD RESET    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_1  ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
				  )
{

/************************
 *     LOCAL DATA	*
 ************************/





/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_HRD_RST_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_HRD_RST_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_HRD_RST_NOPN ;		/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Caller's request valid, save SSP registers that will be used to Reset Modem	*/
slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
slot->clean_up            = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_hard_rst_clean_up ;
oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;

/*Set slot's/channel's control signals to indicate to MPA to Hard Reset Modem Board*/
oregs->cout_ctrl_sigs = 0x1CC ;				/*Issue Reset Modem	*/
slot->frame_ctr       = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
slot->read_toggle     = 0x00 ;				/*UART I/O Read toggle	*/
slot->break_state     = 0x00 ;				/*Indicate Break = OFF	*/
slot->loop_back	      = 0x00 ;				/*Indicate Loop BK = OFF*/

/*Exit function & delay 8 frames thus allowing time for MPA to "see" Modem Reset*/
marb->actv_srvc  = MARB_ACTV_SRVC_HARD_RESET ;		/*Indicate outstnd'g req*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_hard_rst_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 1st function, mpa_srvc_hard_rst_1		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 2nd FUNCTION, ADDRESS UART's IER Register     *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_2 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*8 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" Modem Reset, set up control	signals	*/
/*to address UART's IER								*/
if ( slot->type == 0x01 )				   /*PnP brd be'g Reset?*/ /*R9*/
{							   /*If yes, do't reinit*/
	marb->cont_funct      = mpa_srvc_hard_rst_cmplt;  /*brd since Hard Rst	*/
	return ;					   /*places PnP brd in	*/
}							   /*Wait for Key state	*/

	
oregs->cout_ctrl_sigs = 0x111 ;				/*Addr UART's IER	*/
slot->frame_ctr       = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct      = mpa_srvc_hard_rst_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 2nd function, mpa_srvc_hard_rst_2		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 3rd FUNCTION, INITIALIZE (WRITE TO) UART's IER*
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_3 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x100, gregs, slot ) == 0 )
	return ;					/*256 frames not elapsed*/

/*Have delayed long enough to allow non-UART based Modems to initialize and also*/
/*allowed time for MPA to "see" control signal setting indicating that output be*/
/*directed to UART's Interrupt Enable Register (IER)				*/
oregs->cout_xon_1         = 0x08 ;			/*IER=Modem Stat int ena*/
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till IER output sent						*/
slot->frame_ctr  = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_hard_rst_4 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 3rd function, mpa_srvc_hard_rst_3		*/

/*#page*/
/*******************************************************************************
 *                                                                             *
 * 		HARD RESET MODEM BOARD - 4th FUNCTION, TOGGLE CONTROL SIGNALS  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_4 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*IER char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	mpa_srvc_hard_rst_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
	marb->srvc_status = ERR_HRD_RST_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's IER written to, set up to toggle control signals thus causing MCR to	*/
/* be updated									*/
oregs->cout_ctrl_sigs = ((slot->sv_cout_cntrl_sig & 0x0237) | 0x0004) ^ 0x33 ;
slot->frame_ctr       = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct      = mpa_srvc_hard_rst_5 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 4th function, mpa_srvc_hard_rst_4		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 		HARD RESET MODEM BOARD - 5th FUNCTION, TOGGLE CONTROL SIGNALS  *
 *						       (forcing MPA to wrt/init*
 *							UART's/Modem's MCR)    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_5 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to inverse of control signals, restore	*/
/*Control Toggle signals to assure MCR updated					*/
oregs->cout_ctrl_sigs = ((slot->sv_cout_cntrl_sig & 0x0237) | 0x0004) ;
slot->frame_ctr       = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct      = mpa_srvc_hard_rst_6 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 5th function, mpa_srvc_hard_rst_5		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 6th FUNCTION, ADDRESS UART'S LCR REGISTER     *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_6 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to inverse of control signals, set output 	*/
/*contol signals to address UART's Line Control Register (LCR)			*/
oregs->cout_ctrl_sigs = 0x122 ;			/*R15*/	/*Addr UART's FCB 	*/ 
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_hard_rst_6a ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 6th function, mpa_srvc_hard_rst_6		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 6a FUNCTION, INITIALIZE UART'S FCR REGISTER   *
 *                                                                             *
 *******************************************************************************/
/*R15*/
static void  mpa_srvc_hard_rst_6a ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_5a ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                       marb,  sspcb, slot,		/*Go init FCR		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till FCR Char sent then address LCB				*/
marb->cont_funct = mpa_srvc_hard_rst_6b ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 6a function, mpa_srvc_hard_rst_6a		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 6b FUNCTION, ADDRESS LCR REGISTER   	       *
 *                                                                             *
 *******************************************************************************/
/*R15*/
static void  mpa_srvc_hard_rst_6b ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_5b ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                       marb,  sspcb, slot,		/*Go init FCR		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then address LCR				*/
marb->cont_funct = mpa_srvc_hard_rst_7 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 6b function, mpa_srvc_hard_rst_6b		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 7th FUNCTION, INITIALIZE UART'S LCR REGISTER  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_7 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_6 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                       marb,  sspcb, slot,		/*Go init LCR		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then address DLL				*/
marb->cont_funct = mpa_srvc_hard_rst_8 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 7th function, mpa_srvc_hard_rst_7		*/
/*#page*/
/********************************************************************************
 *                                                                              *
 * 	HARD RESET MODEM BOARD - 8th FUNCTION, ADDRESS UART's DLL, DIVISOR LATCH*
 *				 	       REGISTER (Least Significant byte)*
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_8 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_7 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr DLL reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize DLL							*/
marb->cont_funct = mpa_srvc_hard_rst_9 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 8th function, mpa_srvc_hard_rst_8		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 * 	HARD RESET MODEM BOARD - 9th FUNCTION, INITIALIZE UART'S DLL REGISTER  *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_9 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_8 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                       marb,  sspcb, slot,		/*Go init DLL		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till DLL Char sent then address DLM				*/
marb->cont_funct = mpa_srvc_hard_rst_10 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 9th function, mpa_srvc_hard_rst_9		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    HARD RESET MODEM BOARD - 10th FUNCTION, ADDRESS UART's DIVISOR LATCH     *
 *			REGISTER (Most Significant byte) DLM		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_10 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_9 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr DLM reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize DLM							*/
marb->cont_funct = mpa_srvc_hard_rst_11 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 10th function, mpa_srvc_hard_rst_10		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    HARD RESET MODEM BOARD - 11th FUNCTION, INITIALIZE UART'S DLM REGISTER   *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_11 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_10 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                        marb,  sspcb, slot,		/*Go init DLM		*/
		        gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till DLM Char sent then address LCR (to clr divisor latch ena)	*/
marb->cont_funct = mpa_srvc_hard_rst_12 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 11th function, mpa_srvc_hard_rst_11		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    HARD RESET MODEM BOARD - 12th FUNCTION, ADDRESS UART'S LCR REGISTER      *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_12 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_11 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                        marb,  sspcb, slot,		/*Go set control sig's	*/
		        gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize LCR							*/
marb->cont_funct = mpa_srvc_hard_rst_13 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 12th function, mpa_srvc_hard_rst_12		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  HARD RESET MODEM BOARD - 13th FUNCTION, CLEAR UART'S LCR REGISTER DIVISOR  *
 *                                          LATCH ENABLE BIT		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_13 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_12 ( (unsigned long)MARB_ACTV_SRVC_HARD_RESET,
                        marb,  sspcb, slot,		/*Go init LCR		*/
		        gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till LCR Char sent then set slot's/chan's control signals for	*/
/*normal operation								*/
marb->cont_funct = mpa_srvc_hard_rst_14 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 13th function, mpa_srvc_hard_rst_13		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  		HARD RESET MODEM BOARD - 14th FUNCTION, FLUSH MPA FIFOs	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_14 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*LCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	mpa_srvc_hard_rst_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
	marb->srvc_status = ERR_HRD_RST_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*Modem's LCR written to, place MPA into Reset (FLUSH) state thus flushing	*/
/*internal input/output buffers							*/
oregs->cout_ctrl_sigs = 0x188 ;				/*MPA's Slot Reset	*/
slot->read_toggle     = 0x00 ;				/*UART I/O Read toggle	*/
slot->frame_ctr  = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_hard_rst_15 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 14th function, mpa_srvc_hard_rst_14		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  		HARD RESET MODEM BOARD - 15th FUNCTION, SET CONTROL SIGNALS    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_15 ( unsigned long		        caller_parm,
				   struct marb_struct      *  		marb,
	      			   struct ssp_struct      *   		sspcb,
	      			   struct slot_struct      *  		slot,
	      			   volatile struct icp_gbl_struct      * 	gregs,
				   volatile struct icp_in_struct      *	  	iregs,
	      			   volatile struct icp_out_struct      *	  	oregs
				 )
{

/************************
 *     LOCAL DATA	*
 ************************/




/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x08, gregs, slot ) == 0 )
	return ;					/*8 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that Slot should be reset, set Slot's control signals for normal operation	*/
oregs->cout_ctrl_sigs = (slot->sv_cout_cntrl_sig & 0x0237) | 0x0004 ;
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_hard_rst_cmplt ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*END of Hard Reset Modem - 15th function, mpa_srvc_hard_rst_15		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  	      HARD RESET MODEM BOARD - 16th FUNCTION, REQUEST COMPLETE	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_hard_rst_cmplt ( unsigned long		        caller_parm, /*R9*/
				       struct marb_struct      *  	marb,
	      			       struct ssp_struct      *   	sspcb,
	      			       struct slot_struct      *  	slot,
	      			       volatile struct icp_gbl_struct      * 	gregs,
				       volatile struct icp_in_struct      *	  	iregs,
	      			       volatile struct icp_out_struct      *	oregs
				     )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_HRD_RST_REMOVED ;	/*Set completion status	*/
	return ;
}
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
{
	marb->srvc_state = 0xFF ;			/*Set "call back" state	*/
	return ;					/*4 frames not elapsed	*/
}

/*Have delayed long enough for MPA to "see" control signals, complete request	*/
mpa_srvc_hard_rst_clean_up (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Complete*/

}	/*END of Hard Reset Modem - 16th function, mpa_srvc_hard_rst_cmplt		*/






/*#stitle SLOT SERVICE - HARD RESET MODEM BOARD, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *     	     HARD RESET MODEM BOARD - Clean Up SSP (mpa_srvc_hard_rst_clean_up)		*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion)					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_hard_rst_clean_up (struct slot_struct      *    slot,		*
 *			     	           unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_hard_rst_clean_up (struct slot_struct      *    slot,
					    unsigned char	         type)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

oregs = slot->output ;					/*Slot's output regs ptr*/

/*Delay if actually canceling/aborting an outstanding Hard Reset MARB, thus 	*/
/*allowing for MPA to handle Reset						*/
if ( (type == 0xFF) && (slot->actv_marb->srvc_status == MARB_OUT_CANCELED) )
{
	if ( (oregs->cout_ctrl_sigs == 0x188) ||	/*State of MPA = Flush	*/
	     (oregs->cout_ctrl_sigs == 0x1CC) )		/*   OR Hard Reset	*/
	{						/*If yes		*/
	     /*If abort'g MARB, delay to give MPA chance to FLUSH or Hard Reset	*/
		while ( frame_delay_chk ( (unsigned short int)0x08,
					  slot->actv_marb->sspcb->global, slot)	== 0 )
		{ }
	}
}

/*Restore saved SSP registers							*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x0237) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_hard_rst_clean_up	*/







/*#stitle EXCEPTION SERVICE - INPUT ERROR (mpa_srvc_input_error)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                           INPUT ERROR - SERVICE ENTRY POINT                 *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int	mpa_srvc_input_error (struct marb_struct      *  marb,
						      unsigned char              error_descrpt[] )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/
unsigned char		sv_sup_uchar3 ;			/*Saved MARB sup_uchar3	*/
unsigned short int	ret_status ;			/*Completion status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_SLOT_ERROR_MARB) ;			/*If no, return error	*/
if ( (marb->in_use != 0) || (marb->req_type != 0x00) )	/*Init MARB Req & Waitd?*/
	return (ERR_SLOT_ERROR_MARB) ;			/*If no, return error	*/

/*MARB OK, attempt to determine error						*/
sv_sup_uchar3 = marb->sup_uchar3 ;			/*Save 			*/
marb->cont_funct = mpa_srvc_error_1 ;			/*Init MARB's FN ptr	*/
ret_status       = async_req_hndlr(marb) ;		/*Go handle request	*/
if ( ret_status == 0 )					/*Is Request OK?	*/
{							/*If yes, set ret parms	*/
	error_descrpt[0] = marb->sup_uchar3 ;		/*Error type		*/
	error_descrpt[1] = marb->sup_uchar0 ;		/*Errored character	*/
}
marb->sup_uchar3 = sv_sup_uchar3 ;			/*Restore		*/
return (ret_status) ;					/*Request complete, exit*/
}			/*END of mpa_srvc_input_error Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    INPUT ERROR HANDLING - 1st (and only) Function, Evaluate Error String    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_error_1 ( unsigned long		        caller_parm,
				struct marb_struct      *  	marb,
	      			struct ssp_struct      *   	sspcb,
	      			struct slot_struct      *  	slot,
	      			volatile struct icp_gbl_struct      * gregs,
				volatile struct icp_in_struct      *	iregs,
	      			volatile struct icp_out_struct      *	oregs
				)
{

/************************
 *     LOCAL DATA	*
 ************************/

unsigned char		lsr ;				/*Byte describing error	*/
							/*UART's LSR register	*/

unsigned char		i ;				/*Counter used to determ*/
							/*which LSR bit set	*/


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
gregs       = gregs ;
iregs       = iregs ;
oregs       = oregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Slot occupied?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_SLOT_ERROR_NOT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_SLOT_ERROR_NOPN ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Slot Openned, evaluate requestor's string					*/
lsr = marb->sup_uchar1 ;				/*Get LSR reg		*/
for ( i = 4; i != 0; --i )				/*Determ which error bit*/
{							/*is set		*/
	if ( (lsr & 0x20) == 0x20 )			/*Is error bit set?	*/
		break ;					/*If yes		*/
	lsr <<= 1 ;					/*Shift next error bit	*/
}
if ( i == 0 )						/*Is an error bit set?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_SLOT_ERROR_STRNG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
marb->sup_uchar3 = i - 1 ;				/*Pass error type back	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Complete*/
}	/*End of INPUT ERROR HANDLER - 1st Function, mpa_srvc_error_1		*/











/*#stitle EXCEPTION SERVICE - START BREAK (mpa_srvc_start_break)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                           START BREAK - SERVICE ENTRY POINT                 *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_start_break (struct marb_struct      *  marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_START_BREAK_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->gate = 0x00 ;				/*Set 1st call flag/gate*/
	marb->cont_funct = mpa_srvc_start_break_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_START_BREAK ) /*Valid "Call Back"?*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_START_BREAK_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_start_break Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    	        START BREAK - 1st Function, ADDRESS UART'S LCR REGISTER	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_start_break_1  ( unsigned long		        caller_parm,
				       struct marb_struct      *  	marb,
	      			       struct ssp_struct      *   	sspcb,
	      			       struct slot_struct      *  	slot,
	      			       volatile struct icp_gbl_struct      *  gregs,
				       volatile struct icp_in_struct      *		iregs,
	      			       volatile struct icp_out_struct      *	oregs
				     )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
if ( (marb->gate == 0x00) && (slot->req_outstnd != 0x00) ) /*Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_START_BREAK_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_START_BREAK_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_START_BREAK_NOPN ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Slot Openned, if 1st call to function save SSP registers that will be used to	*/
/*access LCR register and establish "clean up" function in case De-register	*/
/*Modem request handled asynchronously						*/
if ( marb->gate == 0x00 )				/*1st time Fn called?	*/
{							/*If yes		*/
	slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
	slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
	slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
	slot->clean_up   = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_break_clean_up ;
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->actv_srvc  = MARB_ACTV_SRVC_START_BREAK ;	/*Indicate outstnd'g req*/
	marb->gate = 0xFF ;				/*Set for repeat call	*/
	oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;
}

/*Set up to address LCR register so can turn BREAK ON				*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_11 ( (unsigned long)MARB_ACTV_SRVC_START_BREAK,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize LCR							*/
marb->sup_uchar2 = 0x40 ;				/*Indicate SET BREAK	*/
marb->cont_funct = mpa_srvc_break_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/


}		/*End of START BREAK - 1st Function, mpa_srvc_start_break_1	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *   	     BREAK Handling - 2nd Function, INITIALIZE UART'S LCR REGISTER     *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_break_2 ( unsigned long		        caller_parm,
				struct marb_struct      *  	marb,
	      			struct ssp_struct      *   	sspcb,
	      			struct slot_struct      *  	slot,
	      			volatile struct icp_gbl_struct      * gregs,
				volatile struct icp_in_struct      *	iregs,
	      			volatile struct icp_out_struct      *	oregs
			       )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;

unsigned char	sv_break_state ;			/*Slot's svd BREAK State*/


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_break_state    = slot->break_state ;			/*Sv in case need restre*/
slot->break_state = marb->sup_uchar2 ;			/*Set BREAK State	*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_12 ( (unsigned long)MARB_ACTV_SRVC_START_BREAK,
                       marb,  sspcb, slot,		/*Go init LCR		*/
		       gregs, iregs, oregs ) ;
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
{							/*If yes		*/
	slot->break_state = sv_break_state ;		/*Restore BREAK State	*/
	return ;					/*If yes		*/
}

/*Set up & wait till LCR Char sent						*/
marb->cont_funct = mpa_srvc_break_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of START Handling - 2nd FUNCTION, mpa_srvc_break_2		*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *   	          BREAK Handling - 3rd Function, REQUEST COMPLETE	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_break_3  ( unsigned long		        caller_parm,
				 struct marb_struct      *  	marb,
	      			 struct ssp_struct      *   	sspcb,
	      			 struct slot_struct      *  	slot,
	      			 volatile struct icp_gbl_struct      * gregs,
				 volatile struct icp_in_struct      *	 iregs,
	      			 volatile struct icp_out_struct      *	 oregs
				)
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_START_BREAK_REMOVED ;	/*Set completion status	*/
	return ;
}
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*LCR char xmit done	*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		marb->srvc_state = 0xFF ;		/*Set "call back" state	*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_START_BREAK_DEADMAN ; 	/*Set cmpltn status	*/
	return ;
}

/*Modem's LCR written to, complete request					*/
(*slot->clean_up)(slot, (unsigned char)0x00) ;		/*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Complete*/

}	/*End of BREAK Handling - 3rd FUNCTION, mpa_srvc_break_3		*/








/*#stitle EXCEPTION SERVICE - BREAK Handling, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *       BREAK Handling, LOCAL FUNCTION - Clean Up SSP (mpa_srvc_break_clean_up)	*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion)					*
 *											*
 * NOTES: - Function is used by both Start & Stop BREAK					*
 *                                                                     			*
 * CALL: void  mpa_srvc_break_clean_up  (struct slot_struct      *  slot,		*
 *					 unsigned char		    type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_break_clean_up (struct slot_struct      *  slot,
					 unsigned char		    type)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
type = type ;

/*Restore saved SSP registers							*/
oregs = slot->output ;					/*Slot's output regs ptr*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End mpa_srvc_break_clean_up		*/











/*#stitle EXCEPTION SERVICE - STOP BREAK (mpa_srvc_stop_break)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *                            STOP BREAK - SERVICE ENTRY POINT                 *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_stop_break (struct marb_struct      *  marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_STOP_BREAK_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->gate = 0x00 ;				/*Set 1st call flag	*/
	marb->cont_funct = mpa_srvc_stop_break_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_STOP_BREAK ) /*Valid "Call Back"?*/
	{						/*If no, exit		*/
		marb->srvc_status = ERR_STOP_BREAK_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_stop_break Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *    	        STOP BREAK - 1st Function, ADDRESS UART'S LCR REGISTER	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_stop_break_1  ( unsigned long		        caller_parm,
				      struct marb_struct      *  	marb,
	      			      struct ssp_struct      *   	sspcb,
	      			      struct slot_struct      *  	slot,
	      			      volatile struct icp_gbl_struct      *   gregs,
				      volatile struct icp_in_struct      *		iregs,
	      			      volatile struct icp_out_struct      *		oregs
				     )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
if ( (marb->gate == 0x00) && (slot->req_outstnd != 0x00) )  /*Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_STOP_BREAK_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_STOP_BREAK_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_STOP_BREAK_NOPN ;	/*Indicate error	*/
	return ;					/*Exit			*/
}

/*Slot Openned, if 1st call to function save SSP registers that will be used to	*/
/*access LCR register and establish "clean up" function in case De-register	*/
/*Modem request handled asynchronously						*/
if ( marb->gate == 0x00 )				/*1st time Fn called?	*/
{							/*If yes		*/
	slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
	slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
	slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
	slot->clean_up            = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_break_clean_up ;
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->actv_srvc  = MARB_ACTV_SRVC_STOP_BREAK ;	/*Indicate outstnd'g req*/
	marb->gate = 0xFF ;				/*Set for repeat call	*/
	oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;
}

/*Set up to address LCR register so can turn BREAK ON				*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_11 ( (unsigned long)MARB_ACTV_SRVC_STOP_BREAK,
                       marb,  sspcb, slot,		/*Go set control sig's	*/
		       gregs, iregs, oregs ) ;		/*to addr LCR reg	*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & initialize LCR							*/
marb->sup_uchar2 = 0x00 ;				/*Indicate CLEAR BREAK	*/
marb->cont_funct = mpa_srvc_break_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/


}		/*End of STOP BREAK - 1st Function, mpa_srvc_stop_break_1	*/









/*#stitle EXCEPTION SERVICE - MPA<->UART HRDWRE FLOW CONTROL (mpa_srvc_mpa_flow)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *              MPA<->UART HARDWARE FLOW CONTROL - SERVICE ENTRY POINT          *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_mpa_flow (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_MPA_FLOW_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->gate = 0x00 ;				/*Set 1st call flag/gate*/
	marb->cont_funct = mpa_srvc_mpa_flow_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_MPA_FLOW ) /*Valid "Call Back"?  */
	{						/*If no, exit		*/
		marb->srvc_status = ERR_MPA_FLOW_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_mpa_flow Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *      MPA<->UART HARDWARE FLOW CONTROL - 1st FUNCTION, ADDRESS MPA AUX Reg   *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mpa_flow_1  ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
				  )
{

/************************
 *     LOCAL DATA	*
 ************************/





/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( (marb->gate == 0x00) && (slot->req_outstnd != 0x00) ) /*Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_MPA_FLOW_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MPA_FLOW_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MPA_FLOW_NOPN ;		/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( (marb->sup_uchar0 & 0xC0) != 0 )			/*Is Flow parm sane?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_MPA_FLOW_BAD_PARM ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
	
/*Caller's request valid, if 1st call initialize MARB's Slot Control Block	*/
if ( marb->gate == 0x00 )				/*1st time Fn called?	*/
{
	slot->flow_setting = marb->sup_uchar0 ;		/*Save flow setting	*/

   /*Save SSP registers that will be used to establish flow control and establish	*/
   /*"clean up" function in case De-register Modem request handled asynchronously	*/
	slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
	slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
	slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
	slot->clean_up            = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_mpa_flow_clean_up ;
	slot->frame_ctr  = gregs->gicp_frame_ctr ;	/*Snap shot for timing	*/
	marb->actv_srvc  = MARB_ACTV_SRVC_MPA_FLOW ;	/*Indicate outstnd'g req*/
	marb->gate = 0xFF ;				/*Set for repeat call	*/
	oregs->cout_flow_cfg = (oregs->cout_flow_cfg & 0x10) | 0x40 | 0x08 ;
}

/*Set up so output sent to MPA's AUX reg - MPA-to-UART Flow Control		*/
oregs->cout_ctrl_sigs = 0x1AA ;				/*Send control sigs to	*/
							/*MPA indicat'g to direct*/
							/*output to MPA-to-UART	*/
							/*Flow Cntrl		*/

/*Exit function & delay 4 frames thus allowing time for MPA to "see" AUX Reg 	*/
/*control signal setting							*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_mpa_flow_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MPA<->UART FLOW CNTRL - 1st Function, mpa_srvc_mpa_flow_1	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  MPA<->UART HARDWARE FLOW CONTROL - 2nd FUNCTION, INITIALIZE FLOW CONTROL   *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mpa_flow_2   ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

void            (     *sv_cont_funct)(
				      unsigned long int        caller_parm,
				      struct marb_struct            * ,
				      struct ssp_struct             * ,
				      struct slot_struct            * ,
				      volatile struct mpa_global_s           * ,
				      volatile struct mpa_input_s            * ,
				      volatile struct mpa_output_s           * 
				     ) ;


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;

/*Start of function's logic							*/
sv_cont_funct = marb->cont_funct ;			/*Save addr of "link" fn*/
mpa_srvc_reg_modem_4 ( (unsigned long)MARB_ACTV_SRVC_MPA_FLOW,
                       marb,  sspcb, slot,		/*Go send requestor's 	*/
		       gregs, iregs, oregs ) ;		/*MPA-to-UART Flow set'g*/
if ( (marb->srvc_state == 0x00)		   ||		/*Did error occur? or	*/
     (sv_cont_funct    == marb->cont_funct) )		/*Wait'g for frame delay*/
	return ;					/*If yes		*/

/*Set up & wait till MPA AUX Reg Flow Control setting (character) sent		*/
marb->cont_funct = mpa_srvc_mpa_flow_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}	/*End of MPA<->UART FLOW CNTRL - 2nd Function, mpa_srvc_mpa_flow_2	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *  	  MPA<->UART HARDWARE FLOW CONTROL - 3rd FUNCTION, REQUEST COMPLETE    *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_mpa_flow_3   ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Board still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_MPA_FLOW_REMOVED ;	/*Set completion status	*/
	return ;
}

if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*AUX Reg char xmit done*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		marb->srvc_state = 0xFF ;		/*Set "call back" state	*/
		return ;
	}
	(*slot->clean_up)(slot, (unsigned char)0xFF) ;
	marb->srvc_status = ERR_MPA_FLOW_DEADMAN ; 	/*Set cmpltn status	*/
	return ;
}

/*MPA-to-UART Flow Control established, complete request			*/
mpa_srvc_mpa_flow_clean_up  (slot, (unsigned char)0x00 ) ; /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
return ;						/*Exit, Request Cmplt	*/

}	/*End of MPA<->UART FLOW CNTRL - 3rd Function, mpa_srvc_mpa_flow_3	*/










/*#stitle EXCEPTION SERVICE - MPA<->UART HRDWRE FLOW CONTROL, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *	 MPA<->UART HRDWRE FLOW CONTROL - Clean Up SSP (mpa_srvc_mpa_flow_clean_up)	*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion)					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_mpa_flow_clean_up  (struct slot_struct      *    slot,		*
 *					      unsigned char		 type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_mpa_flow_clean_up (struct slot_struct      *    slot,
					      unsigned char		 type)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
type = type ;

/*Restore saved SSP registers							*/
oregs = slot->output ;					/*Slot's output regs ptr*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_mpa_flow_clean_up	*/








/*#stitle EXCEPTION SERVICE - READ UART REGISTER (mpa_srvc_read_uart)#*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *          		READ UART REGISTER - SERVICE ENTRY POINT               *
 *                                                                             *
 *******************************************************************************/

extern unsigned short int   mpa_srvc_read_uart (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/



/************************
 *   START OF CODE	*
 ************************/

status = val_marb (marb) ;				/*Validate caller's MARB*/
if ( status != 0 )					/*Is MARB OK to use?	*/
	return (ERR_RD_UART_MARB) ;			/*If no, return error	*/
if ( marb->in_use == 0 )				/*Initial MARB Request?	*/
{							/*If yes, init Fn ptr	*/
	marb->cont_funct = mpa_srvc_rd_uart_1 ;	/*Init MARB's FN ptr	*/
}
else							/*"Call Back" MARB,	*/
{							/*Validate SRVC active	*/
	if ( marb->actv_srvc != MARB_ACTV_SRVC_READ_UART ) /*Valid "Call Back"? */
	{						/*If no, exit		*/
		marb->srvc_status = ERR_RD_UART_CALL_BK ;
		marb->srvc_state  = 0x00 ;
		return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                               marb->srvc_status) ) ;
	}
}
return ( async_req_hndlr(marb) ) ;			/*Go handle request	*/


}			/*END of mpa_srvc_read_uart Service Entry function	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *             READ UART REGISTER - 1st FUNCTION, SET TO READ UART REGISTER BY *
 *						 ADDRESSING MPA's AUX Reg      *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_rd_uart_1   ( unsigned long		        caller_parm,
				    struct marb_struct      *  		marb,
	      			    struct ssp_struct      *   		sspcb,
	      			    struct slot_struct      *  		slot,
	      			    volatile struct icp_gbl_struct      * 	gregs,
				    volatile struct icp_in_struct      *	  	iregs,
	      			    volatile struct icp_out_struct      *	  	oregs
				  )
{

/************************
 *     LOCAL DATA	*
 ************************/





/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;
iregs       = iregs ;

/*Start of function's logic							*/
if ( slot->req_outstnd != 0x00 )			/*Is a Req Outstand'g?	*/
{							/*If yes, return error	*/
	marb->srvc_status = ERR_RD_UART_REQ_OUT ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->slot_state != 0xFF )				/*Is Modem registered?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_RD_UART_NOT_REG ;	/*Indicate error	*/
	return ;					/*Exit			*/
}
if ( slot->init_state != 0xFF )				/*Is Slot init'd/opn'd?	*/
{							/*If no, error		*/
	marb->srvc_status = ERR_RD_UART_NOPN ;		/*Indicate error	*/
	return ;					/*Exit			*/
}
/*Caller's request valid, save SSP registers that will be used to Read Slot's	*/
/*UART register									*/
slot->sv_cout_flow_config = oregs->cout_flow_cfg ;
slot->sv_cout_cntrl_sig   = oregs->cout_ctrl_sigs ;
slot->sv_cout_xon_1       = oregs->cout_xon_1 ;
slot->clean_up            = (void (*)(struct slot_struct      *, unsigned char))mpa_srvc_rd_uart_clean_up ;

/*Set slot's/channel's control signals to address AUX (UART Read) reg.		*/
oregs->cout_ctrl_sigs = 0x199 ;				/*Addr MPA's AUX reg	*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
oregs->cout_flow_cfg |= 0x08 ;			/*Assure xmit XON char	*/

/*Exit function & delay 4 frames thus allowing time for MPA to "see" AUX Reg 	*/
/*control signal setting							*/
marb->actv_srvc  = MARB_ACTV_SRVC_READ_UART ;		/*Indicate outstnd'g req*/
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
marb->cont_funct = mpa_srvc_rd_uart_2 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of READ UART REG - 1st Function, mpa_srvc_rd_uart_1	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *             READ UART REGISTER - 2nd FUNCTION, WRITE TO MPA's AUX REG       *
 *					  INDICATING TO MPA TO READ AND (XMIT) *
 *					  REQUESTOR's UART REG		       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_rd_uart_2    ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP input area variables							*/
volatile struct cin_bnk_struct     	*actv_bank ;	/*Ptr to active bank		   */


/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_RD_UART_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( frame_delay_chk ( (unsigned short int)0x04, gregs, slot ) == 0 )
	return ;					/*4 frames not elapsed	*/

/*Have delayed long enough for MPA to "see" control signal setting indicating	*/
/*that output be directed to MPA's AUX Register (SPR), save slot's "number of 	*/
/*input characters" so can determine when UART's byte has been received		*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	*/
marb->sup_ushort = actv_bank->bank_num_chars ;		    /*Save	*/

/*Initiate read of requestor's UART register by writing pattern to AUX register	*/
/*indicating to MPA to read Requestor's UART register.  Note: Change clean up	*/
/*function so if abort during read then outstanding read is cleaned up i.e. read*/
/*character accounted for.							*/
slot->clean_up            = (void(*)(struct slot_struct      *, unsigned char))mpa_srvc_rd_uart_cln_rd ;
slot->read_toggle        ^= 0x08 ;			/*Sv I/O Rd Toggle state*/
oregs->cout_xon_1         = marb->sup_uchar0 | 		/*AUX REG byte = RD UART*/
			    slot->read_toggle ; 
oregs->cout_flow_cfg  ^= 0x10 ;			/*Initiate xmit'g char	*/

/*Set up & wait till AUX REG output sent					*/
slot->frame_ctr = gregs->gicp_frame_ctr ;		/*Snap shot for timing	*/
marb->cont_funct = mpa_srvc_rd_uart_3 ;
return ;						/*Exit, thus allowing	*/
							/*non-waited request to	*/
							/*"work mix"		*/

}		/*End of READ UART REG - 2nd Function, mpa_srvc_rd_uart_2	*/
/*#page*/
/*******************************************************************************
 *                                                                             *
 *             READ UART REGISTER - 3rd FUNCTION, WAIT FOR UART REGISTER BYTE  *
 *					 	  TO BE RECIEVED	       *
 *                                                                             *
 *******************************************************************************/

static void  mpa_srvc_rd_uart_3    ( unsigned long		        caller_parm,
				     struct marb_struct      *  	marb,
	      			     struct ssp_struct      *   	sspcb,
	      			     struct slot_struct      *  	slot,
	      			     volatile struct icp_gbl_struct      * 	gregs,
				     volatile struct icp_in_struct      *	  	iregs,
	      			     volatile struct icp_out_struct      *	  	oregs
				   )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP input area variables							*/
volatile struct cin_bnk_struct     	*actv_bank ;	/*Ptr to active bank		   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
caller_parm = caller_parm ;
sspcb       = sspcb ;

/*Start of function's logic							*/
if ( slot->slot_state != 0xFF )				/*Is Brd still present?	*/
{							/*If no, modem pulled	*/
	marb->srvc_status = ERR_RD_UART_REMOVED ;	/*Set completion status	*/
	return ;
}
marb->srvc_state = 0xFF ;				/*Set "call back" state	*/
if ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
     ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) )	/*AUX reg char xmit done*/
{							/*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	mpa_srvc_rd_uart_clean_up (slot, (unsigned char)0xFF) ;			   /*R9*/
	marb->srvc_status = ERR_RD_UART_DEADMAN ; 	/*Set cmpltn status	*/
	marb->srvc_state = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*MPA's AUX Reg Requestor's READ UART Register request sent, wait for UART 	*/
/*Register's byte to be received						*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  */
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  */
if ( actv_bank->bank_num_chars == marb->sup_ushort ) /*Have recv'd UART byte? */
{							 /*If no, check deadman	  */
	if ( frame_delay_chk ( (DEADMAN_DELAY+5), gregs, slot) == 0x00 )
	{						/*If deadman not expired*/
		return ;
	}
	mpa_srvc_rd_uart_clean_up (slot, (unsigned char)0xFF) ;			   /*R9*/
	marb->srvc_status = ERR_RD_UART_INPUT_TO ; 	/*Set cmpltn status	*/
	marb->srvc_state  = 0x00 ;			/*Set "complete" state	*/
	return ;
}

/*UART Register byte received, complete request					*/
mpa_srvc_rd_uart_clean_up (slot, (unsigned char)0x00 ); /*Restore SSP regs	*/
marb->srvc_status = 0x00 ;				/*Set completion status	*/
marb->srvc_state  = 0x00 ;				/*Set completion state	*/
return ;						/*Exit, Request Cmplt	*/
}		/*End of READ UART REG - 3rd Function, mpa_srvc_rd_uart_3	*/









/*#stitle EXCEPTION SERVICE - READ UART REGISTER, LOCAL FUNCTION#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *     		READ UART REGISTER - Clean Up SSP (mpa_srvc_rd_uart_clean_up)		*
 *                                                                     			*
 * PURPOSE: To restore state of SSP registers when service has been completed (either 	*
 *	   due to error or successful completion)					*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_rd_uart_clean_up (struct slot_struct      *    slot,		*
 *			     	          unsigned char		       type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_rd_uart_clean_up (struct slot_struct      *    slot,
					   unsigned char		type)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_out_struct      *	oregs ;		/*Slot's SSP Output regs	   */



/************************
 *   START OF CODE	*
 ************************/

/*Clean up compiler warnings - unused variables (note: should compile as NOPs)	*/
type = type ;

/*Start of function's logic							*/
/*Restore saved SSP registers							*/
oregs = slot->output ;					/*Slot's output regs ptr*/
oregs->cout_flow_cfg = (slot->sv_cout_flow_config & 0x2F) |  /*Rstr out sigs */
                          (oregs->cout_flow_cfg & 0x10) | 0x40 ;
oregs->cout_ctrl_sigs   = (slot->sv_cout_cntrl_sig & 0x02BB) | 0x0004 ;
oregs->cout_xon_1	= slot->sv_cout_xon_1 ;		/*Restore XON char	*/
slot->clean_up          = (void (*)(struct slot_struct      *, unsigned char))0 ;
return ;

}					/*End of mpa_srvc_rd_uart_clean_up	*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *    READ UART REGISTER - Outstanding Read Clean Up SSP (mpa_srvc_rd_uart_cln_rd)	*
 *                                                                     			*
 * PURPOSE: To restore state of the SSP when UART Read outstanding has been aborted.	*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void  mpa_srvc_reg_modem_cln_rd    (struct slot_struct         *slot,		*
 *					     unsigned char		type)		*
 *                                                                     			*
 *			where:	slot = Slot Control Block pointer			*
 *				type = Clean up type:					*
 *					o 0x00 = Normal i.e. Reg Modem request cmplt	*
 *					o 0xFF = Abnormal i.e. Reg Modem request	*
 *						 aborted due to some failure or Board	*
 *						 removed				*
 *                                                                     			*
 *                                                                     			*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

static void	mpa_srvc_rd_uart_cln_rd  (struct slot_struct        	*slot,	   /*R9*/
					  unsigned char			type)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables									*/
volatile struct icp_gbl_struct      * gregs ;	/*Slot's Global regs	*/
volatile struct icp_in_struct      *	 iregs ;	/*Slot's Input regs	*/
volatile struct cin_bnk_struct      *	 actv_bank ;	/*Slot's active Bank	*/
volatile struct icp_out_struct      *	 oregs ;	/*Slot's Output regs	*/

/*Variables use to delay for UART character to be received			*/
unsigned short int			init_frame ; 	/*Initial Frame ctr	*/
unsigned short int			cur_frame_ct ;	/*Frame ctr to calc delta*/

/************************
 *   START OF CODE	*
 ************************/

/*Set Slot's SSP pointer's							*/
gregs	  = slot->global ;				/*Slot's global regs ptr*/
iregs	  = slot->input ;				/*Slot's input regs ptr	*/
oregs	  = slot->output ;				/*Slot's output regs ptr*/
actv_bank = ( (iregs->cin_locks & 0x01) == 0x00 ) 		  ? /*Bank A actv?*/
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_a) : /*If yes	  */
	      (volatile struct cin_bnk_struct      *)&(iregs->cin_bank_b) ; /*If no	  */

/*Assure MPA AUX reg character sent (Read UART toggle)				*/
while ( ((oregs->cout_flow_cfg & 0x10) != (oregs->cout_int_flow_ctrl & 0x10)) ||
        ((oregs->cout_int_flow_ctrl & 0x08) == 0x08) ) /*AUX reg char xmit dne*/
{							 /*If no, check deadman	*/
	if ( frame_delay_chk (DEADMAN_DELAY, gregs, slot) == 0xFF )
		break ;					/*If deadman expired	*/
}

/*MPA's AUX Reg RD UART Reg req sent, delay to allow time for byte to be received*/
init_frame = gregs->gicp_frame_ctr ;			/*Snap shot for delay	*/
while ( 1 == 1 )					/*Delay - allow time for*/
{							/*UART char to be recv'd*/
	cur_frame_ct = gregs->gicp_frame_ctr ;		/*SSP current Frame CTR	*/
	if ( cur_frame_ct < init_frame )		/*Did Frame Ct wrap?	*/
		init_frame = 0 ;  			/*If yes, adjust	*/
	if ( (unsigned short int)(cur_frame_ct - init_frame) >= 6 )
		break ;
}

/*Have cleaned up outstanding read, now restore saved SSP registers by calling	*/
/*"standard" clean up routine.							*/
mpa_srvc_rd_uart_clean_up ( slot, type ) ;		/*Restore sv'd SSP regs	*/
return ;

}					/*End of mpa_srvc_rd_uart_cln_rd	*/





/*#stitle INTERNAL/LOCAL FN - VALIDATE SSP DATA AREA PTR (val_ssp)#*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *               	VALIDATE SSP DATA AREA PTR				*
 *                                                                     		*
 * PURPOSE: To validate SSP Data Area pointer					*
 *										*
 * NOTES:									*
 *                                                                     		*
 * CALL: unsigned char	val_ssp	(struct ssp_struct	*sspcb)			*
 *                                                                     		*
 *			where: sspcb = SSP Data Area pointer to validate	*
 *                                                                     		*
 *                                                                     		*
 * RETURN: Status (unsigned char):						*
 *		o 0x00 = Pointer valid, OK to use				*
 *		o 0xF0 = SSPCB's SSP Global registers not accessable		*
 *		o 0xFF = Pointer invalid, do not use				*
 *                                                                     		*
 ********************************************************************************/

static unsigned char	val_ssp (struct ssp_struct     	*sspcb)

{

/************************
 *     LOCAL DATA	*
 ************************/

volatile struct mpa_global_s            *global_regs ;        	/*SSP's Global reg         */



/************************
 *   START OF CODE	*
 ************************/

if ( (sspcb->in_use    != 0xFF)     ||
     (sspcb->rev       != MPA_REV)  ||
     (sspcb->signature != sspcb) )
	return ( (unsigned char)0xFF ) ;

/*SSP Control Block appears sane, validate that SSP Registers appear "mapped"	*/
global_regs = sspcb->global ;				/*SSPCB's SSP Globl regs*/
if (                                                    /*Does it appear that 	*/
      ((global_regs->gicp_attn & 0xF0) != 0 ) ||	/* SSPCB SSP reg area is*/
      ((global_regs->gicp_chnl & 0xC0) != 0 ) ||	/*  mapped in?		*/
     (((global_regs->gicp_scan_spd) & ~sspcb->sv_sp_mul_msk) != sspcb->sv_sp_mul_set) )
	return ( (unsigned char)0xF0 ) ;		/*If no, error		*/

return ( (unsigned char)0x00 ) ;

}					/*END of val_ssp function		*/

     








/*#stitle INTERNAL/LOCAL FN - VALIDATE MARB (val_marb)#*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *               	       VALIDATE MARB					*
 *                                                                     		*
 * PURPOSE: To validate MPA Async Service Request Block (MARB)			*
 *										*
 * NOTES:									*
 *                                                                     		*
 * CALL: unsigned char	val_marb (struct marb_struct     	*marb)		*
 *                                                                     		*
 *			where: marb = MARB pointer to validate			*
 *                                                                     		*
 *                                                                     		*
 * RETURN: Status (unsigned char):						*
 *		o 0x00 = MARB valid (OK to use)					*
 *		o 0xFF = MARB parameter bad (do not use)			*
 *                                                                     		*
 ********************************************************************************/

static unsigned char	val_marb (struct marb_struct     	*marb)

{

/************************
 *     LOCAL DATA	*
 ************************/



/************************
 *   START OF CODE	*
 ************************/

if ( marb->in_use == 0 )				/*Initial MARB call	*/
{							/*If yes		*/
	if ( (marb->signature  != (struct marb_struct      *)0) 	    ||
	     (marb->cont_funct != (void (*)(unsigned long,
					    struct marb_struct       *,
	      			     	    struct ssp_struct        *,
	      			            struct slot_struct       *,
	      			            volatile struct mpa_global_s      *,
				            volatile struct mpa_input_s       *,
	      			            volatile struct mpa_output_s      *))0) )
		return ((unsigned char)0xFF) ;		/*If MARB bad, ret error*/
}
else							/*MARB being used for	*/
{							/*"call back"		*/
	if ( (marb->signature  != marb) ||
	     (marb->cont_funct == (void (*)(unsigned long,
					    struct marb_struct       *,
	      			     	    struct ssp_struct        *,
	      			            struct slot_struct       *,
	      			            volatile struct mpa_global_s      *,
				            volatile struct mpa_input_s       *,
	      			            volatile struct mpa_output_s      *))0) )
		return ((unsigned char)0xFF) ;		/*If MARB bad, ret error*/
}

/*MARB valid, OK to use, return with good status				*/
return ((unsigned char)0x00) ;

}					/*END of val_marb function		*/



/*#stitle INTERNAL/LOCAL FN - ASYNC SERVICE REQUEST HANDLER (async_req_hndlr)#*/
/*#page*/
/********************************************************************************
 *                                                                             	*
 *               	   ASYNC SERVICE REQUEST HANDLER 			*
 *                                                                     		*
 * PURPOSE: To validate a requestor's MPA Async Request Block (MARB) and if the	*
 *	   MARB is validate "link" to the MARB's Handler function.		*
 *										*
 * NOTES:									*
 *                                                                     		*
 * CALL: unsigned short int   async_req_hndlr  (struct marb_struct     	*marb)	*
 *                                                                     		*
 *			where: marb = MARB pointer to validate			*
 *                                                                     		*
 *                                                                     		*
 * RETURN: Service Request State & Status					*
 *                                                                     		*
 ********************************************************************************/

static unsigned short int   async_req_hndlr  (struct marb_struct       *marb)

{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSPCB (SSP Control Block), MPACB (MPA Control Block) and SLOTCB (Slot 	*/
/*Control Block) variables							*/
struct ssp_struct        	*sspcb ;		/*MARB's SSCB pointer	*/
unsigned int		 	mpa_index ;		/*MARB's MPA/Eng index	*/
struct slot_struct       	*slot ;			/*MARB's SLOT Cntrl Blk	*/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
volatile struct icp_gbl_struct       *gregs ;		/*Request's Global regs	   */
volatile struct icp_in_struct     	       	*iregs ;		/*Request's Input regs	   */
volatile struct icp_out_struct     	*oregs ;		/*Request's Output regs	   */

/*General working variables							*/
unsigned char		status ;			/*Funct. ret status	*/


/************************
 *   START OF CODE	*
 ************************/

marb->srvc_state = 0x00 ;				/*Set MARB's State=done	*/
sspcb = marb->sspcb ;					/*Get MARB's SSPCB's	*/
status = val_ssp ( sspcb ) ;				/*Validate MARB's SSPCB	*/
if ( status != 00 )					/*Is MARB's SSPCB OK?	*/
{							/*If no, exit with error*/
  /*MARB's SSPCB is invalid (not in correct state), since MARB's SSPCB not in 	*/
  /*good shape clean up MARB and return error status				*/
	marb->srvc_status = MARB_STATUS_SSPCB ;		/*MARB's status = bad	*/
	goto COMPLETE_EXIT ;
}
/*MARB's SSPCB valid, set up & get ptr to Request's (MARB's) Slot Control Block	*/
/*and validate SLOTCB ok							*/
mpa_index = marb->slot_chan >> 4 ;			/*Index to Req's MPA/Eng*/
if ( sspcb->mpa[mpa_index].base_chan != 
    ((mpa_index - (sspcb->mpa[mpa_index].eng_num)) << 4) ) /*Is Marb's MPA OK?	*/
{							   /*If no, error	*/
	marb->srvc_status = MARB_STATUS_MPACB ;		/*MARB does not pt to	*/
	goto COMPLETE_EXIT ;				/* valid MPA, error	*/
}
slot = (struct slot_struct       *)&((sspcb->mpa[mpa_index].eng_head)[(marb->slot_chan & 0x0f)]) ;
if ( (slot->in_use   != 0xFF) 		 || 		/*Is MARB's Slot valid	*/
     (slot->alloc    != 0xFF)		 ||
     (slot->chan_num != marb->slot_chan) ||
     (slot->signature->in_use != 0xFF) )
{							/*If no, error		*/
	marb->srvc_status = MARB_STATUS_SLOTCB ;	/*MARB's does not pt to	*/
	goto COMPLETE_EXIT ;				/*valid MPA, error	*/
}

/*MARB's SLOT Control Block ok, point to request's (MARB's) SSP registers	*/
gregs = sspcb->global ;					/*Req's SSP Global regs	*/
iregs = slot->input ;					/*Req's SSP Input regs	*/
oregs = slot->output ;					/*Req's SSP Output regs	*/

/*Determine if 1st request or subsequent "call back" request and validate MARB	*/
if ( marb->in_use == 0 )				/*Is 1st request ?	*/
{							/*If yes		*/
	if ( slot->req_outstnd == 0 )			/*Is a req oustnd?	*/
		slot->clean_up = (void (*)(struct slot_struct      *, unsigned char))0 ;
}
else							/*"Call back" MARB,	*/
{							/*validate request OK	*/
	if ( (slot->req_outstnd == 0x00) ||		/*Does MARB's SLOT	*/
	     (slot->actv_marb   != marb) ||		/* indicate Request	*/
	     (slot->cont_funct  != marb->cont_funct) ||	/*  outstanding and	*/
	     (marb->signature->in_use != 0xFF) )	/*   is MARB OK?	*/
	{						/*SLOT/MARB not set up	*/
							/*  to continue/call bk	*/
		marb->srvc_status = MARB_STATUS_CALL_BK ;
		goto COMPLETE_EXIT ;
	}
}

/*MARB and SLOT etc. valid, call MARB's function and if Request broken into 	*/
/*multiple "call back" functions continue calling if "waited" MARB		*/
do
{
	marb->srvc_state = 0x00 ;			/*Pre-init state = done	*/
	(*marb->cont_funct)(				/*Call MARB's function:	*/
	      (unsigned long)                   0,	/*  - Calling Fn's parm */
	      (struct marb_struct      *)  	marb,	/*  - MARB		*/
	      (struct ssp_struct      *)   	sspcb,	/*  - SSP Control Block	*/
	      (struct slot_struct      *)  	slot,	/*  - SLOT Control Block*/
	      (volatile struct icp_gbl_struct      *) gregs,	/*  - SSP Global Regs	*/
	      (volatile struct icp_in_struct      *)	iregs,	/*  - Slot's Input regs	*/
	      (volatile struct icp_out_struct      *)	oregs	/*  - Slot's Output regs*/
	     ) ;
	if ( marb->srvc_state != 0 )			/*Is MARB completed?	*/
	{						/*If no			*/
		slot->req_outstnd = marb->actv_srvc ;	/*Set MARB's SLOT = 	*/
		slot->actv_marb   = marb ;		/* Request outstanding	*/
	}
}
while ( (marb->srvc_state != 0) && (marb->req_type == 0x00) ) ;

/*Return to to caller, Request is either complete or "call back" required	*/
if ( marb->srvc_state == 0x00 )				/*Is Request complete?	*/
{							/*If yes		*/
	if ( (slot->req_outstnd != 0x00) &&		/*Has outstanding Req 	*/
	     (slot->actv_marb   == marb)  )		/*been completed?	*/
							/*Note: A request that	*/
							/*	was issued while*/
							/*	a req outstndg	*/
							/*	won't take path	*/
	{						/*If yes, clean up	*/
		slot->req_outstnd = 0x00 ;		/*Slot has no REQ Outstd*/
		slot->actv_marb   = (struct marb_struct      *)0 ; /* No actv MARB*/
		slot->cont_funct  = (void (*)(unsigned long,	   /*No "call back"*/
					      struct marb_struct       *,
		      			      struct ssp_struct        *,
		      			      struct slot_struct       *,
		      			      volatile struct mpa_global_s      *,
					      volatile struct mpa_input_s       *,
		      			      volatile struct mpa_output_s      *))0 ;
		slot->clean_up = (void (*)(struct slot_struct      *, unsigned char))0 ;
	}

  /*Common exit point when Async Request is complete due to success or error.	*/
  /*Clean up MARB so it can be used for a new request.				*/
  /*NOTE: Assumes that MARB's Service Status (srvc_status) has been set		*/
COMPLETE_EXIT:
	marb->in_use     = 0x00 ;			/*Indicate avail MARB	*/
	marb->actv_srvc  = 0x00 ;			/*Set no req outstndg	*/
	marb->signature  = (struct marb_struct      *)0; /*Assure MARB invalid	*/
	marb->cont_funct = (void (*)(unsigned long,
				     struct marb_struct       *,
	      			     struct ssp_struct        *,
	      			     struct slot_struct       *,
	      			     volatile struct mpa_global_s      *,
				     volatile struct mpa_input_s       *,
	      			     volatile struct mpa_output_s      *))0 ;
	return ((unsigned short int)(((unsigned short int)(marb->srvc_state)<<8)+
                                       marb->srvc_status) ) ;
}


/*Service not complete & caller's request is (marb->req_type == 0xFF), set up	*/
/*for "call back" 								*/
slot->cont_funct  = marb->cont_funct ;			/*Set up nxt Srvc Fn	*/
marb->in_use      = 0xFF ;				/*Indicate MARB "in_use"*/
marb->signature   = marb ; 				/*Assure MARB valid	*/
marb->srvc_status = 0x00 ;				/*Indicate "call back"	*/
marb->srvc_state  = 0xFF ;				/* required		*/
return ( (unsigned short int)0xFF00 ) ;			/*Return "call back"	*/
							/* completion status	*/

}					/*END of async_req_hndlr function	*/








/*#stitle INTERNAL/LOCAL FN - FRAME DELAY CHECK (frame_delay_chk)#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *               	   FRAME DELAY CHECK						*
 *                                                                     			*
 * PURPOSE: To determine if caller's number of SSP Frames have elapsed.			*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: unsigned char  frame_delay_chk  (unsigned short int              frame_delay,	*
 *		  			  struct icp_gbl_struct        *gregs,	*
 *					  struct slot_struct              *slot)	*
 *                                                                     			*
 *			where:	frame_delay = number of elapsed frames			*
 *				gregs  	    = SSP Global reg pointer			*
 *				slot        = Slot Control Block pointer		*
 *                                                                     			*
 *                                                                     			*
 * RETURN: Number of Elapsed Frames exceeded flag:					*
 *		0x00 = Elapsed # Frames NOT exceeded					*
 *		0xFF = Elasped # Frames exceeded					*
 *                                                                     			*
 ****************************************************************************************/

static unsigned char	frame_delay_chk  (unsigned short int              frame_delay,
	  			  	  volatile struct icp_gbl_struct        *gregs,
				  	  struct slot_struct              *slot
					 )
{

/************************
 *     LOCAL DATA	*
 ************************/

/*SSP variables relating to MARB's/Request's specific SLOT (Channel)		*/
unsigned short int	cur_frame_ct ;			/*Current SSP Frame Ctr	*/



/************************
 *   START OF CODE	*
 ************************/

cur_frame_ct = gregs->gicp_frame_ctr ;			/*SSP current Frame CTR	*/
if ( cur_frame_ct < slot->frame_ctr )			/*Did Frame Ct wrap?	*/
	slot->frame_ctr = 0 ;  				/*If yes, adjust	*/
if ( (unsigned short int)(cur_frame_ct - slot->frame_ctr) < frame_delay )
	return ( (unsigned char)0 ) ;			/*If time has no elapsed*/
return ( (unsigned char)0xFF ) ;			/*If frame ct exceeded	*/

}					/*End of frame_delay_chk function	*/








/*#stitle INTERNAL/LOCAL FN - INCREMENT TAIL POINTER (inc_tail)#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *               	   INCREMENT TAIL POINTER					*
 *                                                                     			*
 * PURPOSE: To update increment slot's/channel's tail pointer thus acknowledging 	*
 *	   receipt of an input character						*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: unsigned char   inc_tail (struct icp_in_struct      *  iregs)			*
 *                                                                     			*
 * RETURN: Function Status:								*
 *		0x00 = Successful							*
 *		0xFF = Failure, invalid buffer size (CIN_Q_CNTRL @ 0x06, Q size > 8)	*
 *                                                                     			*
 ****************************************************************************************/

/************************
 *	GLOBAL DATA	*
 ************************/

/*Global table of masks used to MOD INC Slot's Tail Pointer			*/
static unsigned short int    mask_tbl[] = {		/*Table of masks indexed*/
							/*by Input Queue Size:	*/
					    0x00FF,	/*  Q size = 0 (256)	*/
					    0x01FF,	/*  Q size = 1 (512)	*/
					    0x03FF,	/*  Q size = 2 ( 1K)	*/
					    0x07FF,	/*  Q size = 3 ( 2K)	*/
					    0x0FFF,	/*  Q size = 4 ( 4K)	*/
					    0x1FFF,	/*  Q size = 5 ( 8K)	*/
					    0x3FFF,	/*  Q size = 6 (16K)	*/
					    0x7FFF,	/*  Q size = 7 (32K)	*/
					    0xFFFF	/*  Q size = 8 (64K)	*/
					  } ;


/*#page*/
/************************
 *  FUNCTION ENTRANCE	*
 ************************/

unsigned char	inc_tail (volatile struct icp_in_struct      *  iregs)

{

/************************
 *     LOCAL DATA	*
 ************************/

unsigned char			q_cntrl ;		/*Copy of CIN_Q_CNTRL	*/
unsigned int			mask_index ;		/*Q size - index used to*/
							/*develop masks below	*/
unsigned short int		q_displ_mask ;		/*Mask used to MOD INC	*/
							/*Tail pointer		*/
unsigned short int		actv_tail_value ;	/*Active tail's value	*/

volatile unsigned short int      *non_actv_tail_ptr ;	/*Ptr to non-actv tail	*/

unsigned short int		tail_displ ;		/*Tail Offset	 	*/


/************************
 *   START OF CODE	*
 ************************/

q_cntrl = iregs->cin_q_ctrl ;				/*Get Slot's Q Cntrl reg*/

/*Develop masks used to do MOD INC of tail pointer				*/
mask_index = q_cntrl & 0x0F ;				/*Mask table index	*/
if ( mask_index > 8 )					/*Is Q buffer size OK?	*/
	return ( (unsigned char)0xFF ) ;		/*If no			*/
q_displ_mask = mask_tbl[mask_index] ;			/*Get mask from tbl	*/

/*Determine active tail pointer & get its value					*/
if ( (q_cntrl & 0x10) == 0 )				/*Is tail A active?	*/
{							/*If yes		*/
	actv_tail_value   = iregs->cin_tail_ptr_a ;	/*Get actv tail value	*/
	non_actv_tail_ptr = &(iregs->cin_tail_ptr_b) ;	/*Pt to non-actv tail	*/
}
else							/*If tail B active	*/
{
	actv_tail_value   = iregs->cin_tail_ptr_b ;	/*Get actv tail value	*/
	non_actv_tail_ptr = &(iregs->cin_tail_ptr_a) ;	/*Pt to non-actv tail	*/
}

/*Upate tail pointer by performing MOD INC					*/
tail_displ          = (actv_tail_value + 1) & q_displ_mask  ;	/*Offset + 1	*/
actv_tail_value     = (actv_tail_value & (~q_displ_mask)) | tail_displ ;
*non_actv_tail_ptr  = actv_tail_value ;
iregs->cin_q_ctrl ^= 0x10 ;				/*Change tails		*/
return ( 0 ) ;
}						/*End of INC Tail function	*/








/*#stitle INTERNAL/LOCAL FN - GAIN ACCESS TO SSPCB LIST (mpa_fn_access_sspcb_lst)#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *               	   GAIN ACCESS TO SSPCB LIST					*
 *                                                                     			*
 * PURPOSE: To gain exclusive access to list of Registered SSPCBs by Blocking 		*
 *	   pre-emption and (if appropriate) LOCKING SSPCB List.				*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: unsigned char   mpa_fn_access_sspcb_lst (unsigned long int       *pre_empt_msk)*
 *                                                                     			*
 * INPUT:  pre_empt_msk = pointer for return parm, state of pre-emption (interrupt mask)*
 *			  prior to Blocking pre-emption					*
 *											*
 * RETURN: Function Status:								*
 *		0x00 = Failure, Access to SSPCB NOT granted (pre_empt_msk = invalid)	*
 *		0xFF = Access to SSPCB granted (pre_empt_msk returned)			*
 *                                                                     			*
 ****************************************************************************************/

unsigned char   mpa_fn_access_sspcb_lst (unsigned long int      *pre_empt_msk)

{

/************************
 *     LOCAL DATA	*
 ************************/


/************************
 *   START OF CODE	*
 ************************/

*pre_empt_msk = (*admin_diag_hdr.os_functs.blk_ints_fn)(); /*Block pre-emption	 */
if ( admin_diag_hdr.os_functs.init_lock_fn == (void      *(*)(unsigned long int      *))0 )
{						/*System doesn't support LOCKing */
	return ((unsigned char) 0xFF) ;		/* pre-emption blocked, access to*/
}						/*  SSPCB List granted		 */

/*Pre-emption blocked and system supports LOCKNG, attempt to LOCK SSPCB List	*/
if ( ((*admin_diag_hdr.os_functs.attmpt_lock_fn)(admin_diag_hdr.sspcb_lst_lck_hndl)) == (unsigned char)0xFF )
		return ((unsigned char) 0xFF) ;	/*If Lock granted, grant access	*/

/*Unable to LOCK, restore pre-emption (interrupt) mask and indicated to caller	 */
/*that access to SSPCB List not granted						 */
(*admin_diag_hdr.os_functs.rstr_ints_fn)(*pre_empt_msk) ;
return ((unsigned char) 0x00) ;

}






/*#stitle INTERNAL/LOCAL FN - RELEASE ACCESS TO SSPCB LIST (mpa_fn_relse_sspcb_lst)#*/
/*#page*/
/****************************************************************************************
 *                                                                             		*
 *               	   RELEASE ACCESS TO SSPCB LIST					*
 *                                                                     			*
 * PURPOSE: To rlease exclusive access to list of Registered SSPCBs by clearing (if 	*
 *	   appropriate) SSPCB List LOCK and Clearing Pre-emption Block			*
 *											*
 * NOTES:										*
 *                                                                     			*
 * CALL: void	mpa_fn_relse_sspcb_lst (unsigned long int   pre_empt_msk)		*
 *                                                                     			*
 * INPUT:  pre_empt_msk = state of pre-emption (interrupt mask) prior to pre-emption 	*
 *			  being Blocked (parm returned by mpa_fn_access_sspcb_lst)	*
 *											*
 * RETURN: None										*
 *                                                                     			*
 ****************************************************************************************/

void		mpa_fn_relse_sspcb_lst (unsigned long int   pre_empt_msk) 

{

/************************
 *     LOCAL DATA	*
 ************************/


/************************
 *   START OF CODE	*
 ************************/

if ( admin_diag_hdr.os_functs.init_lock_fn != (void      *(*)(unsigned long int      *))0 )
	(*admin_diag_hdr.os_functs.unlock_fn)(admin_diag_hdr.sspcb_lst_lck_hndl) ;
(*admin_diag_hdr.os_functs.rstr_ints_fn)(pre_empt_msk) ; /*Clear pre-emption blk*/
return ;

}



/*END*/

