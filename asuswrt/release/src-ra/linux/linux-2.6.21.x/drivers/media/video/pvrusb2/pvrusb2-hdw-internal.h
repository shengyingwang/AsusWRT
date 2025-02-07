/*
 *
 *  $Id: pvrusb2-hdw-internal.h,v 1.1.1.1 2010/12/02 04:32:55 walf_wu Exp $
 *
 *  Copyright (C) 2005 Mike Isely <isely@pobox.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#ifndef __PVRUSB2_HDW_INTERNAL_H
#define __PVRUSB2_HDW_INTERNAL_H

/*

  This header sets up all the internal structures and definitions needed to
  track and coordinate the driver's interaction with the hardware.  ONLY
  source files which actually implement part of that whole circus should be
  including this header.  Higher levels, like the external layers to the
  various public APIs (V4L, sysfs, etc) should NOT ever include this
  private, internal header.  This means that pvrusb2-hdw, pvrusb2-encoder,
  etc will include this, but pvrusb2-v4l should not.

*/

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include "pvrusb2-hdw.h"
#include "pvrusb2-io.h"
#include <media/cx2341x.h>

/* Legal values for PVR2_CID_HSM */
#define PVR2_CVAL_HSM_FAIL 0
#define PVR2_CVAL_HSM_FULL 1
#define PVR2_CVAL_HSM_HIGH 2

#define PVR2_VID_ENDPOINT        0x84
#define PVR2_UNK_ENDPOINT        0x86    /* maybe raw yuv ? */
#define PVR2_VBI_ENDPOINT        0x88

#define PVR2_CTL_BUFFSIZE        64

#define FREQTABLE_SIZE 500

#define LOCK_TAKE(x) do { mutex_lock(&x##_mutex); x##_held = !0; } while (0)
#define LOCK_GIVE(x) do { x##_held = 0; mutex_unlock(&x##_mutex); } while (0)

struct pvr2_decoder;

typedef int (*pvr2_ctlf_is_dirty)(struct pvr2_ctrl *);
typedef void (*pvr2_ctlf_clear_dirty)(struct pvr2_ctrl *);
typedef int (*pvr2_ctlf_check_value)(struct pvr2_ctrl *,int);
typedef int (*pvr2_ctlf_get_value)(struct pvr2_ctrl *,int *);
typedef int (*pvr2_ctlf_set_value)(struct pvr2_ctrl *,int msk,int val);
typedef int (*pvr2_ctlf_val_to_sym)(struct pvr2_ctrl *,int msk,int val,
				    char *,unsigned int,unsigned int *);
typedef int (*pvr2_ctlf_sym_to_val)(struct pvr2_ctrl *,
				    const char *,unsigned int,
				    int *mskp,int *valp);
typedef unsigned int (*pvr2_ctlf_get_v4lflags)(struct pvr2_ctrl *);

/* This structure describes a specific control.  A table of these is set up
   in pvrusb2-hdw.c. */
struct pvr2_ctl_info {
	/* Control's name suitable for use as an identifier */
	const char *name;

	/* Short description of control */
	const char *desc;

	/* Control's implementation */
	pvr2_ctlf_get_value get_value;      /* Get its value */
	pvr2_ctlf_get_value get_min_value;  /* Get minimum allowed value */
	pvr2_ctlf_get_value get_max_value;  /* Get maximum allowed value */
	pvr2_ctlf_set_value set_value;      /* Set its value */
	pvr2_ctlf_check_value check_value;  /* Check that value is valid */
	pvr2_ctlf_val_to_sym val_to_sym;    /* Custom convert value->symbol */
	pvr2_ctlf_sym_to_val sym_to_val;    /* Custom convert symbol->value */
	pvr2_ctlf_is_dirty is_dirty;        /* Return true if dirty */
	pvr2_ctlf_clear_dirty clear_dirty;  /* Clear dirty state */
	pvr2_ctlf_get_v4lflags get_v4lflags;/* Retrieve v4l flags */

	/* Control's type (int, enum, bitmask) */
	enum pvr2_ctl_type type;

	/* Associated V4L control ID, if any */
	int v4l_id;

	/* Associated driver internal ID, if any */
	int internal_id;

	/* Don't implicitly initialize this control's value */
	int skip_init;

	/* Starting value for this control */
	int default_value;

	/* Type-specific control information */
	union {
		struct { /* Integer control */
			long min_value; /* lower limit */
			long max_value; /* upper limit */
		} type_int;
		struct { /* enumerated control */
			unsigned int count;       /* enum value count */
			const char **value_names; /* symbol names */
		} type_enum;
		struct { /* bitmask control */
			unsigned int valid_bits; /* bits in use */
			const char **bit_names;  /* symbol name/bit */
		} type_bitmask;
	} def;
};


/* Same as pvr2_ctl_info, but includes storage for the control description */
#define PVR2_CTLD_INFO_DESC_SIZE 32
struct pvr2_ctld_info {
	struct pvr2_ctl_info info;
	char desc[PVR2_CTLD_INFO_DESC_SIZE];
};

struct pvr2_ctrl {
	const struct pvr2_ctl_info *info;
	struct pvr2_hdw *hdw;
};


struct pvr2_decoder_ctrl {
	void *ctxt;
	void (*detach)(void *);
	void (*enable)(void *,int);
	void (*force_reset)(void *);
};

#define PVR2_I2C_PEND_DETECT  0x01  /* Need to detect a client type */
#define PVR2_I2C_PEND_CLIENT  0x02  /* Client needs a specific update */
#define PVR2_I2C_PEND_REFRESH 0x04  /* Client has specific pending bits */
#define PVR2_I2C_PEND_STALE   0x08  /* Broadcast pending bits */

#define PVR2_I2C_PEND_ALL (PVR2_I2C_PEND_DETECT |\
			   PVR2_I2C_PEND_CLIENT |\
			   PVR2_I2C_PEND_REFRESH |\
			   PVR2_I2C_PEND_STALE)

/* Disposition of firmware1 loading situation */
#define FW1_STATE_UNKNOWN 0
#define FW1_STATE_MISSING 1
#define FW1_STATE_FAILED 2
#define FW1_STATE_RELOAD 3
#define FW1_STATE_OK 4

/* Known major hardware variants, keyed from device ID */
#define PVR2_HDW_TYPE_29XXX 0
#define PVR2_HDW_TYPE_24XXX 1

typedef int (*pvr2_i2c_func)(struct pvr2_hdw *,u8,u8 *,u16,u8 *, u16);
#define PVR2_I2C_FUNC_CNT 128

/* This structure contains all state data directly needed to
   manipulate the hardware (as opposed to complying with a kernel
   interface) */
struct pvr2_hdw {
	/* Underlying USB device handle */
	struct usb_device *usb_dev;
	struct usb_interface *usb_intf;

	/* Device type, one of PVR2_HDW_TYPE_xxxxx */
	unsigned int hdw_type;

	/* Video spigot */
	struct pvr2_stream *vid_stream;

	/* Mutex for all hardware state control */
	struct mutex big_lock_mutex;
	int big_lock_held;  /* For debugging */

	void (*poll_trigger_func)(void *);
	void *poll_trigger_data;

	char name[32];

	/* I2C stuff */
	struct i2c_adapter i2c_adap;
	struct i2c_algorithm i2c_algo;
	pvr2_i2c_func i2c_func[PVR2_I2C_FUNC_CNT];
	int i2c_cx25840_hack_state;
	int i2c_linked;
	unsigned int i2c_pend_types;    /* Which types of update are needed */
	unsigned long i2c_pend_mask;    /* Change bits we need to scan */
	unsigned long i2c_stale_mask;   /* Pending broadcast change bits */
	unsigned long i2c_active_mask;  /* All change bits currently in use */
	struct list_head i2c_clients;
	struct mutex i2c_list_lock;

	/* Frequency table */
	unsigned int freqTable[FREQTABLE_SIZE];
	unsigned int freqProgSlot;

	/* Stuff for handling low level control interaction with device */
	struct mutex ctl_lock_mutex;
	int ctl_lock_held;  /* For debugging */
	struct urb *ctl_write_urb;
	struct urb *ctl_read_urb;
	unsigned char *ctl_write_buffer;
	unsigned char *ctl_read_buffer;
	volatile int ctl_write_pend_flag;
	volatile int ctl_read_pend_flag;
	volatile int ctl_timeout_flag;
	struct completion ctl_done;
	unsigned char cmd_buffer[PVR2_CTL_BUFFSIZE];
	int cmd_debug_state;               // Low level command debugging info
	unsigned char cmd_debug_code;      //
	unsigned int cmd_debug_write_len;  //
	unsigned int cmd_debug_read_len;   //

	int flag_ok;            // device in known good state
	int flag_disconnected;  // flag_ok == 0 due to disconnect
	int flag_init_ok;       // true if structure is fully initialized
	int flag_streaming_enabled; // true if streaming should be on
	int fw1_state;          // current situation with fw1

	int flag_decoder_is_tuned;

	struct pvr2_decoder_ctrl *decoder_ctrl;

	// CPU firmware info (used to help find / save firmware data)
	char *fw_buffer;
	unsigned int fw_size;

	// Which subsystem pieces have been enabled / configured
	unsigned long subsys_enabled_mask;

	// Which subsystems are manipulated to enable streaming
	unsigned long subsys_stream_mask;

	// True if there is a request to trigger logging of state in each
	// module.
	int log_requested;

	/* Tuner / frequency control stuff */
	unsigned int tuner_type;
	int tuner_updated;
	unsigned int freqValTelevision;  /* Current freq for tv mode */
	unsigned int freqValRadio;       /* Current freq for radio mode */
	unsigned int freqSlotTelevision; /* Current slot for tv mode */
	unsigned int freqSlotRadio;      /* Current slot for radio mode */
	unsigned int freqSelector;       /* 0=radio 1=television */
	int freqDirty;

	/* Current tuner info - this information is polled from the I2C bus */
	struct v4l2_tuner tuner_signal_info;
	int tuner_signal_stale;

	/* Video standard handling */
	v4l2_std_id std_mask_eeprom; // Hardware supported selections
	v4l2_std_id std_mask_avail;  // Which standards we may select from
	v4l2_std_id std_mask_cur;    // Currently selected standard(s)
	unsigned int std_enum_cnt;   // # of enumerated standards
	int std_enum_cur;            // selected standard enumeration value
	int std_dirty;               // True if std_mask_cur has changed
	struct pvr2_ctl_info std_info_enum;
	struct pvr2_ctl_info std_info_avail;
	struct pvr2_ctl_info std_info_cur;
	struct v4l2_standard *std_defs;
	const char **std_enum_names;

	// Generated string names, one per actual V4L2 standard
	const char *std_mask_ptrs[32];
	char std_mask_names[32][10];

	int unit_number;             /* ID for driver instance */
	unsigned long serial_number; /* ID for hardware itself */

	/* Minor numbers used by v4l logic (yes, this is a hack, as there
	   should be no v4l junk here).  Probably a better way to do this. */
	int v4l_minor_number_video;
	int v4l_minor_number_vbi;
	int v4l_minor_number_radio;

	/* Location of eeprom or a negative number if none */
	int eeprom_addr;

	enum pvr2_config config;

	/* Control state needed for cx2341x module */
	struct cx2341x_mpeg_params enc_cur_state;
	struct cx2341x_mpeg_params enc_ctl_state;
	/* True if an encoder attribute has changed */
	int enc_stale;
	/* True if enc_cur_state is valid */
	int enc_cur_valid;

	/* Control state */
#define VCREATE_DATA(lab) int lab##_val; int lab##_dirty
	VCREATE_DATA(brightness);
	VCREATE_DATA(contrast);
	VCREATE_DATA(saturation);
	VCREATE_DATA(hue);
	VCREATE_DATA(volume);
	VCREATE_DATA(balance);
	VCREATE_DATA(bass);
	VCREATE_DATA(treble);
	VCREATE_DATA(mute);
	VCREATE_DATA(input);
	VCREATE_DATA(audiomode);
	VCREATE_DATA(res_hor);
	VCREATE_DATA(res_ver);
	VCREATE_DATA(srate);
#undef VCREATE_DATA

	struct pvr2_ctld_info *mpeg_ctrl_info;

	struct pvr2_ctrl *controls;
	unsigned int control_cnt;
};

/* This function gets the current frequency */
unsigned long pvr2_hdw_get_cur_freq(struct pvr2_hdw *);

#endif /* __PVRUSB2_HDW_INTERNAL_H */

/*
  Stuff for Emacs to see, in order to encourage consistent editing style:
  *** Local Variables: ***
  *** mode: c ***
  *** fill-column: 75 ***
  *** tab-width: 8 ***
  *** c-basic-offset: 8 ***
  *** End: ***
  */
