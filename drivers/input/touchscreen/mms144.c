/*
 * mms_ts.c - Touchscreen driver for Melfas MMS-series touch controllers
 *
 * Copyright (C) 2011 Google Inc.
 * Author: Dima Zavin <dima@android.com>
 *         Simon Wilson <simonwilson@google.com>
 *
 * ISP reflashing code based on original code from Melfas.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#define DEBUG
/* #define VERBOSE_DEBUG */
#define SEC_TSP_DEBUG
#define SEC_TSP_VERBOSE_DEBUG


/// #define MMS_144_RESET_PIN   //Define it only if the TSP uses RST Pin under the particular model flag

/* #define FORCE_FW_FLASH */
/* #define FORCE_FW_PASS */
/* #define ESD_DEBUG */

#define W1_DUMMY
#define SEC_TSP_FACTORY_TEST
#define SEC_TSP_FW_UPDATE
#define TSP_BUF_SIZE 1024
#define FAIL -1
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>

#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>

#include <linux/i2c/mms144.h>

#include <asm/unaligned.h>
#include "mms_ts_fw.h"

#define MAX_FINGERS		10
#define MAX_WIDTH		30
#define MAX_PRESSURE		255
#define MAX_ANGLE		90
#define MIN_ANGLE		-90

/* Registers */
#define MMS_MODE_CONTROL	0x01
#define MMS_XYRES_HI		0x02
#define MMS_XRES_LO		0x03
#define MMS_YRES_LO		0x04

#define MMS_INPUT_EVENT_PKT_SZ	0x0F
#define MMS_INPUT_EVENT0	0x10
#define FINGER_EVENT_SZ	8

#define MMS_TSP_REVISION	0xF0
#define MMS_HW_REVISION		0xF1
#define MMS_COMPAT_GROUP	0xF2
#define MMS_FW_VERSION		0xF3

enum {
	ISP_MODE_FLASH_ERASE	= 0x59F3,
	ISP_MODE_FLASH_WRITE	= 0x62CD,
	ISP_MODE_FLASH_READ	= 0x6AC9,
};

/* each address addresses 4-byte words */
#define ISP_MAX_FW_SIZE		(0x1F00 * 4)
#define ISP_IC_INFO_ADDR	0x1F00

#ifdef CONFIG_SEC_DVFS
#define TOUCH_BOOSTER			1
#define TOUCH_BOOSTER_OFF_TIME	100
#define TOUCH_BOOSTER_CHG_TIME	200
#endif

#define COVER_OPEN 0
#define COVER_CLOSED 3

#ifdef SEC_TSP_FW_UPDATE

#define WORD_SIZE		4
#define MMS_I2C_VTG_MIN_UV	1800000
#define MMS_I2C_VTG_MAX_UV	1800000
#define MMS_I2C_LOAD_UA	10000

#define ISC_PKT_SIZE			1029
#define ISC_PKT_DATA_SIZE		1024
#define ISC_PKT_HEADER_SIZE		3
#define ISC_PKT_NUM			31

#define ISC_ENTER_ISC_CMD		0x5F
#define ISC_ENTER_ISC_DATA		0x01
#define ISC_CMD				0xAE
#define ISC_ENTER_UPDATE_DATA		0x55
#define ISC_ENTER_UPDATE_DATA_LEN	9
#define ISC_DATA_WRITE_SUB_CMD		0xF1
#define ISC_EXIT_ISC_SUB_CMD		0x0F
#define ISC_EXIT_ISC_SUB_CMD2		0xF0
#define ISC_CHECK_STATUS_CMD		0xAF
#define ISC_CONFIRM_CRC			0x03
#define ISC_DEFAULT_CRC			0xFFFF

#endif

#ifdef SEC_TSP_FACTORY_TEST
#define TX_NUM		26
#define RX_NUM		14
#define NODE_NUM	364 /* 26x14 */

/* VSC(Vender Specific Command)  */
#define MMS_VSC_CMD			0xB0	/* vendor specific command */
#define MMS_VSC_MODE			0x1A	/* mode of vendor */

#define MMS_VSC_CMD_ENTER		0X01
#define MMS_VSC_CMD_CM_DELTA		0X02
#define MMS_VSC_CMD_CM_ABS		0X03
#define MMS_VSC_CMD_EXIT		0X05
#define MMS_VSC_CMD_INTENSITY		0X04
#define MMS_VSC_CMD_RAW			0X06
#define MMS_VSC_CMD_REFER		0X07

#define TSP_CMD_STR_LEN 32
#define TSP_CMD_RESULT_STR_LEN 512
#define TSP_CMD_PARAM_NUM 8
#endif /* SEC_TSP_FACTORY_TEST */

#if defined(CONFIG_MACH_STRETTO) || defined(CONFIG_MACH_SUPERIORLTE_SKT)
#define ISC_DL_MODE 0
#else
#define ISC_DL_MODE 1
#endif

#define MMS_COORDS_ARR_SIZE	4

#if ISC_DL_MODE
/* Default configuration of ISC mode */

#define DEFAULT_SLAVE_ADDR	0x48

#define SECTION_NUM		3
#define SECTION_NAME_LEN	5

#define PAGE_HEADER		3
#define PAGE_DATA		1024
#define PAGE_TAIL		2
#define PACKET_SIZE		(PAGE_HEADER + PAGE_DATA + PAGE_TAIL)
#define TS_WRITE_REGS_LEN		1030

#define TIMEOUT_CNT		10
#define STRING_BUF_LEN		100


/* State Registers */
#define MIP_ADDR_INPUT_INFORMATION	0x01

#define ISC_ADDR_VERSION		0xE1
#define ISC_ADDR_SECTION_PAGE_INFO	0xE5

/* Config Update Commands */
#define ISC_CMD_ENTER_ISC		0x5F
#define ISC_CMD_ENTER_ISC_PARA1		0x01
#define ISC_CMD_UPDATE_MODE		0xAE
#define ISC_SUBCMD_ENTER_UPDATE		0x55
#define ISC_SUBCMD_DATA_WRITE		0XF1
#define ISC_SUBCMD_LEAVE_UPDATE_PARA1	0x0F
#define ISC_SUBCMD_LEAVE_UPDATE_PARA2	0xF0
#define ISC_CMD_CONFIRM_STATUS		0xAF

#define ISC_STATUS_UPDATE_MODE		0x01
#define ISC_STATUS_CRC_CHECK_SUCCESS	0x03

#define ISC_CHAR_2_BCD(num)	(((num/10)<<4) + (num%10))
#define ISC_MAX(x, y)		(((x) > (y)) ? (x) : (y))

static const char section_name[SECTION_NUM][SECTION_NAME_LEN] = {
	"BOOT", "CORE", "CONF"
};

static const unsigned char crc0_buf[31] = {
	0x1D, 0x2C, 0x05, 0x34, 0x95, 0xA4, 0x8D, 0xBC,
	0x59, 0x68, 0x41, 0x70, 0xD1, 0xE0, 0xC9, 0xF8,
	0x3F, 0x0E, 0x27, 0x16, 0xB7, 0x86, 0xAF, 0x9E,
	0x7B, 0x4A, 0x63, 0x52, 0xF3, 0xC2, 0xEB
};

static const unsigned char crc1_buf[31] = {
	0x1E, 0x9C, 0xDF, 0x5D, 0x76, 0xF4, 0xB7, 0x35,
	0x2A, 0xA8, 0xEB, 0x69, 0x42, 0xC0, 0x83, 0x01,
	0x04, 0x86, 0xC5, 0x47, 0x6C, 0xEE, 0xAD, 0x2F,
	0x30, 0xB2, 0xF1, 0x73, 0x58, 0xDA, 0x99
};

struct mms_bin_hdr
{
	char tag[8];
	u16 core_version;
	u16 section_num;
	u16 contains_full_binary;
	u16 reserved0;

	u32 binary_offset;
	u32 binary_length;

	u32 extention_offset;
	u32 reserved1;

}__attribute__ ((packed));

struct mms_fw_img
{
	u16 type;
	u16 version;

	u16 start_page;
	u16 end_page;

	u32 offset;
	u32 length;

}__attribute__ ((packed));

struct mms_firmware
{
	u8 *data;
	size_t size;
};


typedef enum {
	ISC_NONE = -1,
	ISC_SUCCESS = 0,
	ISC_FILE_OPEN_ERROR,
	ISC_FILE_CLOSE_ERROR,
	ISC_FILE_FORMAT_ERROR,
	ISC_WRITE_BUFFER_ERROR,
	ISC_I2C_ERROR,
	ISC_UPDATE_MODE_ENTER_ERROR,
	ISC_CRC_ERROR,
	ISC_VALIDATION_ERROR,
	ISC_COMPATIVILITY_ERROR,
	ISC_UPDATE_SECTION_ERROR,
	ISC_SLAVE_ERASE_ERROR,
	ISC_SLAVE_DOWNLOAD_ERROR,
	ISC_DOWNLOAD_WHEN_SLAVE_IS_UPDATED_ERROR,
	ISC_INITIAL_PACKET_ERROR,
	ISC_NO_NEED_UPDATE_ERROR,
	ISC_LIMIT
} eISCRet_t;

typedef enum {
	EC_NONE = -1,
	EC_DEPRECATED = 0,
	EC_BOOTLOADER_RUNNING = 1,
	EC_BOOT_ON_SUCCEEDED = 2,
	EC_ERASE_END_MARKER_ON_SLAVE_FINISHED = 3,
	EC_SLAVE_DOWNLOAD_STARTS = 4,
	EC_SLAVE_DOWNLOAD_FINISHED = 5,
	EC_2CHIP_HANDSHAKE_FAILED = 0x0E,
	EC_ESD_PATTERN_CHECKED = 0x0F,
	EC_LIMIT
} eErrCode_t;

typedef enum {
	SEC_NONE = -1,
	SEC_BOOTLOADER = 0,
	SEC_CORE,
	SEC_CONFIG,
	SEC_LIMIT
} eSectionType_t;

typedef struct tISCFWInfo_t {
	unsigned char version;
	unsigned char compatible_version;
	unsigned char start_addr;
	unsigned char end_addr;
	int bin_offset;
	u32 crc;
} tISCFWInfo_t;

//static tISCFWInfo_t mbin_info[SECTION_NUM];
static tISCFWInfo_t mfsb_info[SECTION_NUM];

static tISCFWInfo_t ts_info[SECTION_NUM]; /* read F/W version from IC */
static bool section_update_flag[SECTION_NUM];

const struct firmware *fw_mbin;

static struct mms_firmware fw_mfsb;
static struct mms_bin_hdr *fw_hdr;
static struct mms_fw_img **img;

static unsigned char g_wr_buf[1024 + 3 + 2];
#endif

int touch_is_pressed;
EXPORT_SYMBOL(touch_is_pressed);

enum fw_flash_mode {
	ISP_FLASH,
	ISC_FLASH,
};

enum {
	BUILT_IN = 0,
	UMS,
};

struct tsp_callbacks {
	void (*inform_charger)(struct tsp_callbacks *tsp_cb, bool mode);
};

struct mms_ts_info {
	struct i2c_client		*client;
	struct input_dev		*input_dev;
	char				phys[32];

	int				max_x;
	int				max_y;

	bool				invert_x;
	bool				invert_y;
	const u8			*config_fw_version;
	int				irq;
	struct regulator *vcc_i2c;
	struct mms_ts_platform_data	*pdata;

	char				*fw_name;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend		early_suspend;
#endif
#ifdef TOUCH_BOOSTER
#if TOUCH_BOOSTER
	struct delayed_work work_dvfs_off;
	struct delayed_work	work_dvfs_chg;
	bool	dvfs_lock_status;
	struct mutex dvfs_lock;
#endif
#endif

	/* protects the enabled flag */
	struct mutex			lock;
	bool				enabled;

	enum fw_flash_mode		fw_flash_mode;
	void (*register_cb)(void *);
	struct tsp_callbacks callbacks;
	bool			ta_status;
	bool			noise_mode;

#if defined(SEC_TSP_DEBUG) || defined(SEC_TSP_VERBOSE_DEBUG)
	unsigned char finger_state[MAX_FINGERS];
#endif

#if defined(SEC_TSP_FW_UPDATE)
	u8				fw_update_state;
#endif
u8				fw_ic_ver;

#if defined(SEC_TSP_FACTORY_TEST)
	struct list_head			cmd_list_head;
	u8			cmd_state;
	char			cmd[TSP_CMD_STR_LEN];
	int			cmd_param[TSP_CMD_PARAM_NUM];
	char			cmd_result[TSP_CMD_RESULT_STR_LEN];
	struct mutex			cmd_lock;
	bool			cmd_is_running;

	unsigned int reference[NODE_NUM];
	unsigned int raw[NODE_NUM]; /* CM_ABS */
	unsigned int inspection[NODE_NUM];/* CM_DELTA */
	unsigned int intensity[NODE_NUM];
	bool ft_flag;
	int cover_state;
#endif /* SEC_TSP_FACTORY_TEST */
};

struct mms_fw_image {
	__le32 hdr_len;
	__le32 data_len;
	__le32 fw_ver;
	__le32 hdr_ver;
	u8 data[0];
} __packed;

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mms_ts_early_suspend(struct early_suspend *h);
static void mms_ts_late_resume(struct early_suspend *h);
#endif

#define USE_OPEN_CLOSE

#ifdef USE_OPEN_CLOSE
static void  melfas_ts_close(struct input_dev *dev);
static int  melfas_ts_open(struct input_dev *dev);
#endif

#if defined(SEC_TSP_FACTORY_TEST)
#define TSP_CMD(name, func) .cmd_name = name, .cmd_func = func

struct tsp_cmd {
	struct list_head	list;
	const char	*cmd_name;
	void	(*cmd_func)(void *device_data);
};

static void fw_update(void *device_data);
static void get_fw_ver_bin(void *device_data);
static void get_fw_ver_ic(void *device_data);
static void get_config_ver(void *device_data);
static void get_threshold(void *device_data);
static void module_off_master(void *device_data);
static void module_on_master(void *device_data);
static void get_chip_vendor(void *device_data);
static void get_chip_name(void *device_data);
static void get_reference(void *device_data);
static void get_cm_abs(void *device_data);
static void get_cm_delta(void *device_data);
static void get_intensity(void *device_data);
static void get_x_num(void *device_data);
static void get_y_num(void *device_data);
static void run_reference_read(void *device_data);
static void run_cm_abs_read(void *device_data);
static void run_cm_delta_read(void *device_data);
static void run_intensity_read(void *device_data);
static void not_support_cmd(void *device_data);
static void clear_cover_mode(void *device_data);

struct tsp_cmd tsp_cmds[] = {
	{TSP_CMD("fw_update", fw_update),},
	{TSP_CMD("get_fw_ver_bin", get_fw_ver_bin),},
	{TSP_CMD("get_fw_ver_ic", get_fw_ver_ic),},
	{TSP_CMD("get_config_ver", get_config_ver),},
	{TSP_CMD("get_threshold", get_threshold),},
	{TSP_CMD("module_off_master", module_off_master),},
	{TSP_CMD("module_on_master", module_on_master),},
	{TSP_CMD("module_off_slave", not_support_cmd),},
	{TSP_CMD("module_on_slave", not_support_cmd),},
	{TSP_CMD("get_chip_vendor", get_chip_vendor),},
	{TSP_CMD("get_chip_name", get_chip_name),},
	{TSP_CMD("get_x_num", get_x_num),},
	{TSP_CMD("get_y_num", get_y_num),},
	{TSP_CMD("get_reference", get_reference),},
	{TSP_CMD("get_cm_abs", get_cm_abs),},
	{TSP_CMD("get_cm_delta", get_cm_delta),},
	{TSP_CMD("get_intensity", get_intensity),},
	{TSP_CMD("run_reference_read", run_reference_read),},
	{TSP_CMD("run_cm_abs_read", run_cm_abs_read),},
	{TSP_CMD("run_cm_delta_read", run_cm_delta_read),},
	{TSP_CMD("run_intensity_read", run_intensity_read),},
	{TSP_CMD("not_support_cmd", not_support_cmd),},
	{TSP_CMD("clear_cover_mode", clear_cover_mode),},
};
#endif

#if 0 /* firmware call removal */
static int melfas_mux_fw_flash(struct mms_ts_platform_data *pdata,
				bool to_gpios)
{
	if (to_gpios) {
		gpio_direction_output(pdata->gpio_int, 0);
		gpio_tlmm_config(GPIO_CFG(pdata->gpio_int, 0,
			GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), 1);
		gpio_direction_output(pdata->gpio_scl, 0);
		gpio_tlmm_config(GPIO_CFG(pdata->gpio_scl, 0,
			GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), 1);
		gpio_direction_output(pdata->gpio_sda, 0);
		gpio_tlmm_config(GPIO_CFG(pdata->gpio_sda, 0,
			GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), 1);
	} else {
		gpio_direction_output(pdata->gpio_int, 1);
		gpio_tlmm_config(GPIO_CFG(pdata->gpio_int, 0,
			GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), 1);
		gpio_direction_output(pdata->gpio_scl, 1);
		gpio_tlmm_config(GPIO_CFG(pdata->gpio_scl, 3,
			GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), 1);
		gpio_direction_output(pdata->gpio_sda, 1);
		gpio_tlmm_config(GPIO_CFG(pdata->gpio_sda, 3,
			GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), 1);
	}
	return 0;
}
#endif /* firmware call removal */

#if 0
static int reg_set_optimum_mode_check(struct regulator *reg, int load_uA)
{
	return (regulator_count_voltages(reg) > 0) ?
		regulator_set_optimum_mode(reg, load_uA) : 0;
}
#endif

void  melfas_vdd_on(struct mms_ts_info *info, bool onoff)
{
	int ret = 0, rc = 0;
	pr_info("[TSP] power %s\n", onoff ? "on" : "off");

	if (!info->vcc_i2c) {
		if (info->pdata->i2c_pull_up) {
			info->vcc_i2c = regulator_get(&info->client->dev,
				"vdd_ldo2");
			if (IS_ERR(info->vcc_i2c)) {
				rc = PTR_ERR(info->vcc_i2c);
				dev_err(&info->client->dev,
					"Regulator get failed rc=%d\n", rc);
				goto error_get_vtg_i2c;
			}
			/*
			if (regulator_count_voltages(info->vcc_i2c) > 0) {
				rc = regulator_set_voltage(info->vcc_i2c,
				MMS_I2C_VTG_MIN_UV, MMS_I2C_VTG_MAX_UV);
				if (rc) {
					dev_err(&info->client->dev,
					"regulator set_vtg failed rc=%d\n",
					rc);
					goto error_set_vtg_i2c;
				}
			}
			*/
		}
	}

	if (onoff) {
		if (info->pdata->i2c_pull_up) {
			/*
			rc = reg_set_optimum_mode_check(info->vcc_i2c,
						MMS_I2C_LOAD_UA);
			if (rc < 0) {
				dev_err(&info->client->dev,
				"Regulator vcc_i2c set_opt failed rc=%d\n",
				rc);
				goto error_reg_opt_i2c;
			}
			*/

			rc = regulator_enable(info->vcc_i2c);
			if (rc) {
				dev_err(&info->client->dev,
				"Regulator vcc_i2c enable failed rc=%d\n",
				rc);
			//	goto error_reg_en_vcc_i2c;
			}
		}
	} else {
		if (info->pdata->i2c_pull_up) {
			//reg_set_optimum_mode_check(info->vcc_i2c, 0);
			regulator_disable(info->vcc_i2c);
		}
	}
	//msleep(50);

	/*
	ret = gpio_direction_output(info->pdata->vdd_en, onoff);
	if (ret) {
		pr_err("[TSP]%s: unable to set_direction for mms_vdd_en [%d]\n",
				__func__, info->pdata->vdd_en);
	}
	*/
	if (onoff) {
		ret = regulator_enable(info->pdata->vdd_en_reg);
		if (ret) {
			pr_err("[TSP]%s: unable to enable for mms_vdd_en_reg [%p]\n",
					__func__, info->pdata->vdd_en_reg);
		}
	}
	else {
		regulator_disable(info->pdata->vdd_en_reg);
		if (ret) {
			pr_err("[TSP]%s: unable to disable for mms_vdd_en_reg [%p]\n",
					__func__, info->pdata->vdd_en_reg);
		}
	}
	//msleep(30);
	return;
#if 0
error_reg_en_vcc_i2c:
	if (info->pdata->i2c_pull_up)
		reg_set_optimum_mode_check(info->vcc_i2c, 0);
//error_reg_opt_i2c:
//error_set_vtg_i2c:
	regulator_put(info->vcc_i2c);
#endif
error_get_vtg_i2c:
	return;

}

int is_melfas_vdd_on(struct mms_ts_info *info)
{
	int ret;

	//ret = gpio_get_value(info->pdata->vdd_en);
	ret = regulator_is_enabled(info->pdata->vdd_en_reg);
	pr_info("[TSP] %s = %d\n", __func__, ret);

	if (ret)
		return 1;
	return 0;
}

#ifdef TOUCH_BOOSTER
#if TOUCH_BOOSTER
static void change_dvfs_lock(struct work_struct *work)
{
	struct mms_ts_info *info = container_of(work,
				struct mms_ts_info, work_dvfs_chg.work);
	int ret;

	mutex_lock(&info->dvfs_lock);
	ret = set_freq_limit(DVFS_TOUCH_ID, 998400);
	mutex_unlock(&info->dvfs_lock);

	if (ret < 0)
		pr_err("%s: 1booster stop failed(%d)\n",\
					__func__, __LINE__);
	else
		pr_info("[TSP] %s", __func__);
}

static void set_dvfs_off(struct work_struct *work)
{
	struct mms_ts_info *info = container_of(work,
				struct mms_ts_info, work_dvfs_off.work);
	mutex_lock(&info->dvfs_lock);
	set_freq_limit(DVFS_TOUCH_ID, -1);
	info->dvfs_lock_status = false;
	mutex_unlock(&info->dvfs_lock);
	//pr_info("[TSP] DVFS Off!");
}

static void set_dvfs_lock(struct mms_ts_info *info, uint32_t on)
{
	int ret = 0;

	mutex_lock(&info->dvfs_lock);
	if (on == 0) {
		if (info->dvfs_lock_status) {
			queue_delayed_work(system_power_efficient_wq, &info->work_dvfs_off,
				msecs_to_jiffies(TOUCH_BOOSTER_OFF_TIME));
		}
	} else if (on == 1) {
		cancel_delayed_work(&info->work_dvfs_off);
		if (!info->dvfs_lock_status) {
			ret = set_freq_limit(DVFS_TOUCH_ID, 998400);
			if (ret < 0)
				printk(KERN_ERR "%s: cpu lock failed(%d)\n",\
							__func__, ret);

			info->dvfs_lock_status = true;
			//pr_info("[TSP] DVFS On!");
		}
	} else if (on == 2) {
		cancel_delayed_work(&info->work_dvfs_off);
		schedule_work(&info->work_dvfs_off.work);
	}
	mutex_unlock(&info->dvfs_lock);
}
#endif
#endif

static void release_all_fingers(struct mms_ts_info *info)
{
#ifdef SEC_TSP_DEBUG
	struct i2c_client *client = info->client;
#endif
	int i;

	printk(KERN_DEBUG "[TSP] %s\n", __func__);

	for (i = 0; i < MAX_FINGERS; i++) {
#ifdef SEC_TSP_DEBUG
		if (info->finger_state[i] == 1) {
			dev_notice(&client->dev, "finger %d up(force)\n", i);
		}
#endif
		info->finger_state[i] = 0;
		input_mt_slot(info->input_dev, i);
		input_mt_report_slot_state(info->input_dev, MT_TOOL_FINGER,
					   false);
	}
	input_sync(info->input_dev);
#ifdef TOUCH_BOOSTER
#if TOUCH_BOOSTER
	set_dvfs_lock(info, 2);
	pr_info("[TSP] dvfs_lock free.\n ");
#endif
#endif
}

static void mms_set_noise_mode(struct mms_ts_info *info)
{
	struct i2c_client *client = info->client;

	if (!(info->noise_mode && info->enabled))
		return;
	dev_notice(&client->dev, "%s\n", __func__);

	if (info->ta_status) {
		dev_notice(&client->dev, "noise_mode & TA connect!!!\n");
		i2c_smbus_write_byte_data(info->client, 0x30, 0x1);
	} else {
		dev_notice(&client->dev, "noise_mode & TA disconnect!!!\n");
		i2c_smbus_write_byte_data(info->client, 0x30, 0x2);
	}
}

static inline void mms_pwr_on_reset(struct mms_ts_info *info);

static void reset_mms_ts(struct mms_ts_info *info)
{
	struct i2c_client *client = info->client;
	if (info->enabled == false)
		return;

	dev_notice(&client->dev, "%s++\n", __func__);
	disable_irq_nosync(info->irq);
	info->enabled = false;
	touch_is_pressed = 0;

	release_all_fingers(info);

	mms_pwr_on_reset(info);
	enable_irq(info->irq);
	info->enabled = true;

	if (info->ta_status) {
		dev_notice(&client->dev, "TA connect!!!\n");
		i2c_smbus_write_byte_data(info->client, 0x33, 0x1);
	} else {
		dev_notice(&client->dev, "TA disconnect!!!\n");
		i2c_smbus_write_byte_data(info->client, 0x33, 0x2);
		mms_set_noise_mode(info);
	}

	dev_notice(&client->dev, "%s--\n", __func__);
}

static void melfas_ta_cb(struct tsp_callbacks *cb, bool ta_status)
{
	struct mms_ts_info *info =
			container_of(cb, struct mms_ts_info, callbacks);
	struct i2c_client *client = info->client;

	dev_notice(&client->dev, "%s\n", __func__);

	info->ta_status = ta_status;

	if (info->enabled) {
		if (info->ta_status) {
			dev_notice(&client->dev, "TA connect!!!\n");
			i2c_smbus_write_byte_data(info->client, 0x33, 0x1);
		} else {
			dev_notice(&client->dev, "TA disconnect!!!\n");
			i2c_smbus_write_byte_data(info->client, 0x33, 0x2);
		}
		mms_set_noise_mode(info);
	}
}

struct tsp_callbacks *charger_callbacks;
void tsp_charger_infom(bool en)
{
	if (charger_callbacks && charger_callbacks->inform_charger)
		charger_callbacks->inform_charger(charger_callbacks, en);
}
static void melfas_register_callback(void *cb)
{
	charger_callbacks = cb;
	pr_info("melfas-ts : melfas_register_callback");
}
static irqreturn_t mms_ts_interrupt(int irq, void *dev_id)
{
	struct mms_ts_info *info = dev_id;
	struct i2c_client *client = info->client;
	u8 buf[MAX_FINGERS*FINGER_EVENT_SZ] = { 0 };
	int ret;
	int i;
	int sz;
	u8 reg = MMS_INPUT_EVENT0;
	struct i2c_msg msg[] = {
		{
			.addr   = client->addr,
			.flags  = 0,
			.buf    = &reg,
			.len    = 1,
		}, {
			.addr   = client->addr,
			.flags  = I2C_M_RD,
			.buf    = buf,
		},
	};

	sz = i2c_smbus_read_byte_data(client, MMS_INPUT_EVENT_PKT_SZ);
	if (sz < 0) {
		dev_err(&client->dev, "%s bytes=%d\n", __func__, sz);
		for (i = 0; i < 50; i++) {
			sz = i2c_smbus_read_byte_data(client,
						MMS_INPUT_EVENT_PKT_SZ);
			if (sz > 0)
				break;
		}

		if (i == 50) {
			dev_dbg(&client->dev, "i2c failed... reset!!\n");
			reset_mms_ts(info);
			goto out;
		}
	}
	/* BUG_ON(sz > MAX_FINGERS*FINGER_EVENT_SZ); */
	if (sz == 0)
		goto out;

	if (sz > MAX_FINGERS*FINGER_EVENT_SZ) {
		dev_err(&client->dev, "[TSP] abnormal data inputed.\n");
		goto out;
	}

	msg[1].len = sz;
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret != ARRAY_SIZE(msg)) {
		dev_err(&client->dev,
			"failed to read %d bytes of touch data (%d)\n",
			sz, ret);
		goto out;
	}

#if defined(VERBOSE_DEBUG)
	print_hex_dump(KERN_DEBUG, "mms_ts raw: ",
		DUMP_PREFIX_OFFSET, 32, 1, buf, sz, false);

#endif
	if (buf[0] == 0x0F) { /* ESD */
		dev_dbg(&client->dev, "ESD DETECT.... reset!!\n");
		reset_mms_ts(info);
		goto out;
	}

	if (buf[0] == 0x0E) { /* NOISE MODE */
		dev_dbg(&client->dev, "[TSP] noise mode enter!!\n");
		info->noise_mode = 1 ;
		mms_set_noise_mode(info);
		goto out;
	}

	for (i = 0; i < sz; i += FINGER_EVENT_SZ) {
		u8 *tmp = &buf[i];
		int id = (tmp[0] & 0xf) - 1;
		int x = tmp[2] | ((tmp[1] & 0xf) << 8);
		int y = tmp[3] | (((tmp[1] >> 4) & 0xf) << 8);
		int angle = (tmp[5] >= 127) ? (-(256 - tmp[5])) : tmp[5];
		int palm = (buf[0] & 0x10) >> 4;
		if (info->invert_x) {
			x = info->max_x - x;
			if (x < 0)
				x = 0;
		}
		if (info->invert_y) {
			y = info->max_y - y;
			if (y < 0)
				y = 0;
		}
		if (id >= MAX_FINGERS) {
			dev_notice(&client->dev, \
				"finger id error [%d]\n", id);
			reset_mms_ts(info);
			goto out;
		}

		if ((tmp[0] & 0x80) == 0) {
#if defined(SEC_TSP_DEBUG)
			dev_dbg(&client->dev,
				"finger id[%d]: x=%d y=%d p=%d w=%d major=%d minor=%d angle=%d palm=%d\n"
				, id, x, y, tmp[5], tmp[4], tmp[6], tmp[7]
				, angle, palm);
#else
			dev_notice(&client->dev, "finger [%d] up\n", id);
#endif
			input_mt_slot(info->input_dev, id);
			input_mt_report_slot_state(info->input_dev,
						   MT_TOOL_FINGER, false);

#if defined(SEC_TSP_DEBUG) || defined(SEC_TSP_VERBOSE_DEBUG)
			info->finger_state[id] = 0;
#endif
			continue;
		}

		input_mt_slot(info->input_dev, id);
		input_mt_report_slot_state(info->input_dev,
					   MT_TOOL_FINGER, true);
		input_report_abs(info->input_dev, ABS_MT_WIDTH_MAJOR, tmp[4]);
#if defined(CONFIG_MACH_M2_KDI)
		input_report_abs(info->input_dev, ABS_MT_POSITION_X,
			(info->max_x - x));
		input_report_abs(info->input_dev, ABS_MT_POSITION_Y,
			(info->max_y - y));
#else
		input_report_abs(info->input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(info->input_dev, ABS_MT_POSITION_Y, y);
#endif
		input_report_abs(info->input_dev, ABS_MT_TOUCH_MAJOR, tmp[6]);
		input_report_abs(info->input_dev, ABS_MT_TOUCH_MINOR, tmp[7]);
//		input_report_abs(info->input_dev, ABS_MT_ANGLE, angle);
		input_report_abs(info->input_dev, ABS_MT_PALM, palm);
#if defined(SEC_TSP_DEBUG)
		if (info->finger_state[id] == 0) {
			info->finger_state[id] = 1;
			dev_dbg(&client->dev,
				"finger id[%d]: x=%d y=%d p=%d w=%d major=%d minor=%d angle=%d palm=%d\n"
				, id, x, y, tmp[5], tmp[4], tmp[6], tmp[7]
				, angle, palm);
		}
#else
		if (info->finger_state[id] == 0) {
			info->finger_state[id] = 1;
			dev_notice(&client->dev, "finger [%d] down\n", id);
		}
#endif
	}

	input_sync(info->input_dev);
	touch_is_pressed = 0;

	for (i = 0; i < MAX_FINGERS; i++) {
		if (info->finger_state[i] == 1)
			touch_is_pressed++;
	}

#ifdef TOUCH_BOOSTER
#if TOUCH_BOOSTER
	set_dvfs_lock(info, !!touch_is_pressed);
#endif
#endif

out:
	return IRQ_HANDLED;
}

#if ISC_DL_MODE
static int mms100_i2c_read(struct i2c_client *client,
		u16 addr, u16 length, u8 *value)
{
	struct i2c_adapter *adapter = client->adapter;
	struct i2c_msg msg;
	int ret = -1;

	msg.addr = client->addr;
	msg.flags = 0x00;
	msg.len = 1;
	msg.buf = (u8 *) &addr;

	ret = i2c_transfer(adapter, &msg, 1);

	if (ret >= 0) {
		msg.addr = client->addr;
		msg.flags = I2C_M_RD;
		msg.len = length;
		msg.buf = (u8 *) value;

		ret = i2c_transfer(adapter, &msg, 1);
	}

	if (ret < 0)
		pr_err("[TSP] : read error : [%d]", ret);

	return ret;
}

static int mms100_i2c_write(struct i2c_client *client, char *buf, int length)
{
	int i;
	char *data;
	data = kzalloc(sizeof(char)*TS_WRITE_REGS_LEN, GFP_KERNEL);
	if (length > TS_WRITE_REGS_LEN)
	{
		pr_err("[TSP] %s :size error \n", __FUNCTION__);
		return -EINVAL;
	}

	for (i = 0; i < length; i++)
		data[i] = *buf++;

	i = i2c_master_send(client, (char *) data, length);

	if (i == length){
		kfree(data);
		return length;
	}
	else
	{
		kfree(data);
		pr_err("[TSP] :write error : [%d]", i);
		return -EIO;
	}
}

static int mms100_reset(struct mms_ts_info *info)
{
	melfas_vdd_on(info, 0);
	msleep(30);
	melfas_vdd_on(info, 1);
	msleep(300);

	return ISC_SUCCESS;
}
/*
static eISCRet_t mms100_check_operating_mode(struct i2c_client *_client, const eErrCode_t _error_code)
{
	int ret;
	unsigned char rd_buf = 0x00;

	pr_info("[TSP ISC] %s\n", __func__);

	// Config versionÀ» ÀÐ¾î¼­ booting È®ÀÎ...
	ret = mms100_i2c_read(_client, ISC_ADDR_VERSION, 1, &rd_buf);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c read fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return _error_code;
	}

	pr_info("End mms100_check_operating_mode()\n");

	return ISC_SUCCESS;
}
*/

static eISCRet_t mms100_get_version_info(struct i2c_client *_client)
{
	int i, ret;
	unsigned char rd_buf[8];

	pr_info("[TSP ISC] %s\n", __func__);

	// config version brust read (core, private, public)
	ret = mms100_i2c_read(_client, ISC_ADDR_VERSION, SECTION_NUM, rd_buf);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c read fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	for (i = 0; i < SECTION_NUM; i++)
		ts_info[i].version = rd_buf[i];

	ts_info[SEC_CORE].compatible_version = ts_info[SEC_BOOTLOADER].version;
	ts_info[SEC_CONFIG].compatible_version = ts_info[SEC_CORE].version;

	ret = mms100_i2c_read(_client, ISC_ADDR_SECTION_PAGE_INFO, 8, rd_buf);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c read fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	for (i = 0; i < SECTION_NUM; i++)
	{
		ts_info[i].start_addr = rd_buf[i];

		/*
		 *  previous core binary had 4 sections while current version contains 3 of them
		 * for compatibleness, register address was not modified so we get 1,2,3,5,6,7th
		 * data of read buffer
		 */
		ts_info[i].end_addr = rd_buf[i + SECTION_NUM + 1];
	}

	for (i = 0; i < SECTION_NUM; i++)
	{
		pr_info("\tTS : Section(%d) version: 0x%02X\n", i, ts_info[i].version);
		pr_info("\tTS : Section(%d) Start Address: 0x%02X\n", i, ts_info[i].start_addr);
		pr_info("\tTS : Section(%d) End Address: 0x%02X\n", i, ts_info[i].end_addr);
		pr_info("\tTS : Section(%d) Compatibility: 0x%02X\n", i, ts_info[i].compatible_version);
	}

	pr_info("End mms100_get_version_info()\n");

	return ISC_SUCCESS;
}

static eISCRet_t mms100_seek_section_info(void)
{
#define STRING_BUF_LEN		100

	int i;
	int offset = sizeof(struct mms_bin_hdr);

	pr_info("[TSP ISC] %s\n", __func__);

	fw_hdr = (struct mms_bin_hdr *) fw_mfsb.data;
	img = kzalloc(sizeof(*img) * fw_hdr->section_num, GFP_KERNEL);
	for (i = 0; i < fw_hdr->section_num; i++, offset += sizeof(struct mms_fw_img))
	{
		img[i] = (struct mms_fw_img *) (fw_mfsb.data + offset);
		mfsb_info[i].version = img[i]->version;
		mfsb_info[i].start_addr = img[i]->start_page;
		mfsb_info[i].end_addr = img[i]->end_page;
		if (i == 0)
		{
			mfsb_info[i].compatible_version = img[i]->version;
		}
		else
		{
			mfsb_info[i].compatible_version = img[i - 1]->version;
		}
	}

	for (i = 0; i < SECTION_NUM; i++)
	{
		pr_info("\tMFSB : Section(%d) Version: 0x%02X\n", i, mfsb_info[i].version);
		pr_info("\tMFSB : Section(%d) Start Address: 0x%02X\n", i, mfsb_info[i].start_addr);
		pr_info("\tMFSB : Section(%d) End Address: 0x%02X\n", i, mfsb_info[i].end_addr);
		pr_info("\tMFSB : Section(%d) Compatibility: 0x%02X\n", i, mfsb_info[i].compatible_version);
	}

	pr_info("End mms100_seek_section_info()\n");

	return ISC_SUCCESS;
}


static eISCRet_t mms100_compare_version_info(struct i2c_client *_client)
{
	int i;
	//unsigned char expected_compatibility[SECTION_NUM];
	int target_ver[SECTION_NUM];
	int fw_up_to_date = true;

	pr_info("[TSP ISC] %s\n", __func__);

	if (mms100_get_version_info(_client) != ISC_SUCCESS)
		return ISC_I2C_ERROR;

	mms100_seek_section_info();

	for (i = 0; i < SECTION_NUM; i++)
	{

		if (mfsb_info[i].version != ts_info[i].version)
		{
			fw_up_to_date = false;
			section_update_flag[i] = true;
			target_ver[i] = mfsb_info[i].version;

			if (mfsb_info[0].version != ts_info[0].version)
			{
				section_update_flag[0] = true;
				section_update_flag[1] = true;
				section_update_flag[2] = true;
			}

			if (mfsb_info[1].version != ts_info[1].version)
			{
				section_update_flag[0] = false;
				section_update_flag[1] = true;
				section_update_flag[2] = true;
			}

		}
		else
		{
			target_ver[i] = ts_info[i].version;
		}
	}

	if (fw_up_to_date)
	{
		pr_info("mms_ts firmware version is up to date\n");
		return ISC_NO_NEED_UPDATE_ERROR;
	}

	for (i = 1; i < SECTION_NUM; i++)
	{
		if (target_ver[i - 1] != mfsb_info[i].compatible_version)
		{
			pr_info("compatibility version mismatch(%d), 0x%02x, 0x%02x\n", i, target_ver[i - 1], mfsb_info[i].compatible_version);
			return ISC_COMPATIVILITY_ERROR;
		}
	}

	pr_info("End mms100_compare_version_info()\n");

	return ISC_SUCCESS;
}

static eISCRet_t mms100_enter_ISC_mode(struct i2c_client *_client)
{
	int ret;
	unsigned char wr_buf[2];

	pr_info("[TSP ISC] %s\n", __func__);

	wr_buf[0] = ISC_CMD_ENTER_ISC; // command
	wr_buf[1] = ISC_CMD_ENTER_ISC_PARA1; // sub_command

	ret = mms100_i2c_write(_client, wr_buf, 2);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c write fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	//mms100_msdelay(50);
	mdelay(50);

	pr_info("End mms100_enter_ISC_mode()\n");

	return ISC_SUCCESS;
}

static eISCRet_t mms100_enter_config_update(struct i2c_client *_client)
{
	int ret;
	unsigned char wr_buf[10] =
	{ 0, };
	unsigned char rd_buf;

	pr_info("[TSP ISC] %s\n", __func__);

	wr_buf[0] = ISC_CMD_UPDATE_MODE;
	wr_buf[1] = ISC_SUBCMD_ENTER_UPDATE;

	ret = mms100_i2c_write(_client, wr_buf, 10);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c write fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	ret = mms100_i2c_read(_client, ISC_CMD_CONFIRM_STATUS, 1, &rd_buf);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c read fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	if (rd_buf != ISC_STATUS_UPDATE_MODE)
		return ISC_UPDATE_MODE_ENTER_ERROR;

	pr_info("End mms100_enter_config_update()\n");

	return ISC_SUCCESS;
}

static eISCRet_t mms100_ISC_clear_page(struct i2c_client *_client, unsigned char _page_addr)
{
	int ret;
	unsigned char rd_buf;

	pr_info("[TSP ISC] %s\n", __func__);
	memset(&g_wr_buf[3], 0xFF, PAGE_DATA);

	g_wr_buf[0] = ISC_CMD_UPDATE_MODE; // command
	g_wr_buf[1] = ISC_SUBCMD_DATA_WRITE; // sub_command
	g_wr_buf[2] = _page_addr;

	g_wr_buf[PAGE_HEADER + PAGE_DATA] = crc0_buf[_page_addr];
	g_wr_buf[PAGE_HEADER + PAGE_DATA + 1] = crc1_buf[_page_addr];

	ret = mms100_i2c_write(_client, g_wr_buf, PACKET_SIZE);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c write fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	ret = mms100_i2c_read(_client, ISC_CMD_CONFIRM_STATUS, 1, &rd_buf);

	if (ret < 0)
	{
		pr_info("[TSP ISC] %s,%d: i2c read fail[%d] \n", __FUNCTION__, __LINE__, ret);
		return ISC_I2C_ERROR;
	}

	if (rd_buf != ISC_STATUS_CRC_CHECK_SUCCESS)
		return ISC_UPDATE_MODE_ENTER_ERROR;

	pr_info("End mms100_ISC_clear_page()\n");

	return ISC_SUCCESS;

}

static eISCRet_t mms100_ISC_clear_validate_markers(struct i2c_client *_client)
{
	eISCRet_t ret_msg;
	int i, j;
	bool is_matched_address;

	pr_info("[TSP ISC] %s\n", __func__);

	for (i = SEC_CORE; i <= SEC_CONFIG; i++)
	{
		if (section_update_flag[i])
		{
			if (ts_info[i].end_addr <= 30 && ts_info[i].end_addr > 0)
			{
				ret_msg = mms100_ISC_clear_page(_client, ts_info[i].end_addr);

				if (ret_msg != ISC_SUCCESS)
					return ret_msg;
			}
		}
	}

	for (i = SEC_CORE; i <= SEC_CONFIG; i++)
	{
		if (section_update_flag[i])
		{
			is_matched_address = false;
			for (j = SEC_CORE; j <= SEC_CONFIG; j++)
			{
				if (mfsb_info[i].end_addr == ts_info[i].end_addr)
				{
					is_matched_address = true;
					break;
				}
			}

			if (!is_matched_address)
			{
				if (mfsb_info[i].end_addr <= 30 && mfsb_info[i].end_addr > 0)
				{
					ret_msg = mms100_ISC_clear_page(_client, mfsb_info[i].end_addr);

					if (ret_msg != ISC_SUCCESS)
						return ret_msg;
				}
			}
		}
	}

	pr_info("End mms100_ISC_clear_validate_markers()\n");

	return ISC_SUCCESS;
}

static void mms100_calc_crc(unsigned char *crc, int page_addr, unsigned char* ptr_fw)
{
	int i, j;

	unsigned char ucData;

	unsigned short SeedValue;
	unsigned short CRC_check_buf;
	unsigned short CRC_send_buf;
	unsigned short IN_data;
	unsigned short XOR_bit_1;
	unsigned short XOR_bit_2;
	unsigned short XOR_bit_3;

	// Seed

	CRC_check_buf = 0xFFFF;
	SeedValue = (unsigned short) page_addr;

	for (i = 7; i >= 0; i--)
	{
		IN_data = (SeedValue >> i) & 0x01;
		XOR_bit_1 = (CRC_check_buf & 0x0001) ^ IN_data;
		XOR_bit_2 = XOR_bit_1 ^ (CRC_check_buf >> 11 & 0x01);
		XOR_bit_3 = XOR_bit_1 ^ (CRC_check_buf >> 4 & 0x01);
		CRC_send_buf = (XOR_bit_1 << 4) | (CRC_check_buf >> 12 & 0x0F);
		CRC_send_buf = (CRC_send_buf << 7) | (XOR_bit_2 << 6) | (CRC_check_buf >> 5 & 0x3F);
		CRC_send_buf = (CRC_send_buf << 4) | (XOR_bit_3 << 3) | (CRC_check_buf >> 1 & 0x0007);
		CRC_check_buf = CRC_send_buf;
	}

	for (i = 0; i < 1024; i++)
	{
		ucData = ptr_fw[i];

		for (j = 7; j >= 0; j--)
		{
			IN_data = (ucData >> j) & 0x0001;
			XOR_bit_1 = (CRC_check_buf & 0x0001) ^ IN_data;
			XOR_bit_2 = XOR_bit_1 ^ (CRC_check_buf >> 11 & 0x01);
			XOR_bit_3 = XOR_bit_1 ^ (CRC_check_buf >> 4 & 0x01);
			CRC_send_buf = (XOR_bit_1 << 4) | (CRC_check_buf >> 12 & 0x0F);
			CRC_send_buf = (CRC_send_buf << 7) | (XOR_bit_2 << 6) | (CRC_check_buf >> 5 & 0x3F);
			CRC_send_buf = (CRC_send_buf << 4) | (XOR_bit_3 << 3) | (CRC_check_buf >> 1 & 0x0007);
			CRC_check_buf = CRC_send_buf;
		}
	}

	crc[0] = (unsigned char) ((CRC_check_buf >> 8) & 0xFF);
	crc[1] = (unsigned char) ((CRC_check_buf >> 0) & 0xFF);
}

static eISCRet_t mms100_update_section_data(struct i2c_client *_client)
{
#define STRING_BUF_LEN		100

	int i, j, ret; // 2012.08.30
	unsigned char rd_buf;
	unsigned char crc[2]; // 2012.08.30
	int ptr;
	const u8 *ptr_fw;
	int page_addr;
	const u8 *fw_data;
	pr_info("[TSP ISC] %s\n", __func__);

	fw_data = (u8 *)fw_mfsb.data + fw_hdr->binary_offset;
	for (i = 0; i < fw_hdr->section_num; i++)
	{
		pr_info("update flag (%d)\n", section_update_flag[i]);
		ptr = img[i]->offset;
		if (section_update_flag[i])
		{
			ptr_fw = fw_data + ptr;
			pr_info("binary found\n");

			for (page_addr = mfsb_info[i].start_addr; page_addr <= mfsb_info[i].end_addr; page_addr++)
			{
				if (page_addr - mfsb_info[i].start_addr > 0)
					ptr_fw += 1024;

				g_wr_buf[0] = ISC_CMD_UPDATE_MODE;
				g_wr_buf[1] = ISC_SUBCMD_DATA_WRITE;
				g_wr_buf[2] = (unsigned char) page_addr;

				for (j = 0; j < 1024; j += 4)
				{
					g_wr_buf[3 + j] = ptr_fw[j + 3];
					g_wr_buf[3 + j + 1] = ptr_fw[j + 2];
					g_wr_buf[3 + j + 2] = ptr_fw[j + 1];
					g_wr_buf[3 + j + 3] = ptr_fw[j + 0];
				}

				mms100_calc_crc(crc, page_addr, &g_wr_buf[3]);

				g_wr_buf[1027] = crc[0];
				g_wr_buf[1028] = crc[1];

				pr_info("crc val : %X%X\n", crc[0], crc[1]);
				ret = mms100_i2c_write(_client, g_wr_buf, PACKET_SIZE);

				if (ret < 0)
				{
					pr_info("[TSP ISC] %s,%d: i2c write fail[%d] \n", __FUNCTION__, __LINE__, ret);
					return ISC_I2C_ERROR;
				}

				ret = mms100_i2c_read(_client, ISC_CMD_CONFIRM_STATUS, 1, &rd_buf);

				if (ret < 0)
				{
					pr_info("[TSP ISC] %s,%d: i2c read fail[%d] \n", __FUNCTION__, __LINE__, ret);
					return ISC_I2C_ERROR;
				}

				if (rd_buf != ISC_STATUS_CRC_CHECK_SUCCESS)
					return ISC_CRC_ERROR;

				section_update_flag[i] = false;
				pr_info("section(%d) updated.\n", i);
			}
		}
	}

	pr_info("End mms100_update_section_data()\n");

	return ISC_SUCCESS;
}

static int mms100_open_mbinary(struct i2c_client *_client)
{
	int ret = 0;

	/*
	ret += request_firmware(&(fw_mbin[1]),
		"tsp_melfas/s3/CORE.fw", &_client->dev);
	ret += request_firmware(&(fw_mbin[2]),
		"tsp_melfas/s3/PRIV.fw", &_client->dev);
	ret += request_firmware(&(fw_mbin[3]),
		"tsp_melfas/s3/PUBL.fw", &_client->dev);
	*/

	ret = request_firmware(&fw_mbin,
		"tsp_melfas/DIABLO_CoreV54_V35_master.fw", &_client->dev);

	if (!ret) {
		fw_mfsb.data = kzalloc(fw_mbin->size, GFP_KERNEL);
		memcpy(fw_mfsb.data, fw_mbin->data, fw_mbin->size);
		fw_mfsb.size = fw_mbin->size;
		return ISC_SUCCESS;
	}
	else {
		pr_info("[TSP ISC] request_firmware fail");
		return ISC_FILE_OPEN_ERROR;
	}
}

static int mms100_close_mbinary(void)
{
	/*
	int i;

	for (i = 0; i < SECTION_NUM; i++) {
		if (fw_mbin[i] != NULL)
			release_firmware(fw_mbin[i]);
	}
	*/
	release_firmware(fw_mbin);
	kfree(img);
	kfree(fw_mfsb.data);

	return ISC_SUCCESS;
}

eISCRet_t mms100_ISC_download_mbinary(struct mms_ts_info *info)
{
	struct i2c_client *_client = info->client;
	int ret_msg = ISC_NONE;
	bool force_update = 0;

	pr_info("[TSP ISC] %s\n", __func__);

	mms100_reset(info);
/*
	ret_msg = mms100_check_operating_mode(_client, EC_BOOT_ON_SUCCEEDED);
	if (ret_msg != ISC_SUCCESS)
		goto ISC_ERROR_HANDLE;
*/
	ret_msg = mms100_open_mbinary(_client);
	if (ret_msg != ISC_SUCCESS)
		goto ISC_ERROR_HANDLE;

	/* Config version Check */
	if (force_update)
	{
		int i;
		ret_msg = mms100_compare_version_info(_client);

		for (i = 0; i < SECTION_NUM; i++)
			section_update_flag[i] = true;
	}
	else
	{
		ret_msg = mms100_compare_version_info(_client);
		if (ret_msg != ISC_SUCCESS)
			goto ISC_ERROR_HANDLE;
	}

	ret_msg = mms100_enter_ISC_mode(_client);
	if (ret_msg != ISC_SUCCESS)
		goto ISC_ERROR_HANDLE;

	ret_msg = mms100_enter_config_update(_client);
	if (ret_msg != ISC_SUCCESS)
		goto ISC_ERROR_HANDLE;

	ret_msg = mms100_ISC_clear_validate_markers(_client);
	if (ret_msg != ISC_SUCCESS)
		goto ISC_ERROR_HANDLE;

	pr_info("[TSP ISC]mms100_update_section_data start");

	ret_msg = mms100_update_section_data(_client);
	if (ret_msg != ISC_SUCCESS)
		goto ISC_ERROR_HANDLE;

	pr_info("[TSP ISC]mms100_update_section_data end");

	pr_info("[TSP ISC]FIRMWARE_UPDATE_FINISHED!!!\n");

	ret_msg = ISC_SUCCESS;

ISC_ERROR_HANDLE:
	if(ret_msg == ISC_NO_NEED_UPDATE_ERROR){
		ret_msg = ISC_SUCCESS;
	}else if (ret_msg != ISC_SUCCESS){
		pr_info("ISC_ERROR_CODE: %d\n", ret_msg);
	}

	mms100_reset(info);
	mms100_close_mbinary();

	return ret_msg;
}
#endif	/* ISC_DL_MODE start */

static void hw_reboot(struct mms_ts_info *info, bool bootloader)
{
	melfas_vdd_on(info, 0);
	gpio_direction_output(info->pdata->gpio_sda, bootloader ? 0 : 1);
	gpio_direction_output(info->pdata->gpio_scl, bootloader ? 0 : 1);
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif

	msleep(30);
	melfas_vdd_on(info, 1);
	msleep(30);

	if (bootloader) {
		gpio_set_value(info->pdata->gpio_scl, 0);
		gpio_set_value(info->pdata->gpio_sda, 1);
	} else {
#ifdef MMS_144_RESET_PIN
		printk("[TSP] %s : Reset pin Set Value 1 \n",__func__);
		gpio_set_value(info->pdata->gpio_resetb, 1);
		printk("[TSP] %s : reset pin direction input \n",__func__);
		gpio_direction_input(info->pdata->gpio_resetb);
#endif
		gpio_direction_input(info->pdata->gpio_scl);
		gpio_direction_input(info->pdata->gpio_sda);
	}
	msleep(40);
}

static inline void hw_reboot_bootloader(struct mms_ts_info *info)
{
	hw_reboot(info, true);
}

static inline void hw_reboot_normal(struct mms_ts_info *info)
{
	hw_reboot(info, false);
}

static inline void mms_pwr_on_reset(struct mms_ts_info *info)
{
#if 0 /* firmware call removal */
	struct i2c_adapter *adapter = to_i2c_adapter(info->client->dev.parent);

/*
	if (!info->pdata->mux_fw_flash) {
		dev_info(&info->client->dev,
			 "missing platform data, can't do power-on-reset\n");
		return;
	}
*/
	i2c_lock_adapter(adapter);
	melfas_mux_fw_flash(info->pdata, true);

	melfas_vdd_on(info, 0);
	gpio_direction_output(info->pdata->gpio_sda, 1);
	gpio_direction_output(info->pdata->gpio_scl, 1);
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 1 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 1);
#endif
	msleep(50);
	melfas_vdd_on(info, 1);
	msleep(50);

	melfas_mux_fw_flash(info->pdata, false);
	i2c_unlock_adapter(adapter);

	/* TODO: Seems long enough for the firmware to boot.
	 * Find the right value */
#endif /* fw call removal */
	msleep(250);
}

#if 0 /* firmware call removal */
static void isp_toggle_clk(struct mms_ts_info *info, int start_lvl, int end_lvl,
			   int hold_us)
{
	gpio_set_value(info->pdata->gpio_scl, start_lvl);
	udelay(hold_us);
	gpio_set_value(info->pdata->gpio_scl, end_lvl);
	udelay(hold_us);
}

/* 1 <= cnt <= 32 bits to write */
static void isp_send_bits(struct mms_ts_info *info, u32 data, int cnt)
{
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif
	gpio_direction_output(info->pdata->gpio_scl, 0);
	gpio_direction_output(info->pdata->gpio_sda, 0);

	/* clock out the bits, msb first */
	while (cnt--) {
		gpio_set_value(info->pdata->gpio_sda, (data >> cnt) & 1);
		udelay(3);
		isp_toggle_clk(info, 1, 0, 3);
	}
}

/* 1 <= cnt <= 32 bits to read */
static u32 isp_recv_bits(struct mms_ts_info *info, int cnt)
{
	u32 data = 0;
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif
	gpio_direction_output(info->pdata->gpio_scl, 0);
	gpio_set_value(info->pdata->gpio_sda, 0);
	gpio_direction_input(info->pdata->gpio_sda);

	/* clock in the bits, msb first */
	while (cnt--) {
		isp_toggle_clk(info, 0, 1, 1);
		data = (data << 1) | (!!gpio_get_value(info->pdata->gpio_sda));
	}

	gpio_direction_output(info->pdata->gpio_sda, 0);
	return data;
}

static void isp_enter_mode(struct mms_ts_info *info, u32 mode)
{
	int cnt;
	unsigned long flags;

	local_irq_save(flags);
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0\n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif
	gpio_direction_output(info->pdata->gpio_scl, 0);
	gpio_direction_output(info->pdata->gpio_sda, 1);

	mode &= 0xffff;
	for (cnt = 15; cnt >= 0; cnt--) {
#ifdef MMS_144_RESET_PIN
		printk("[TSP] %s : Reset pin Set Value  \n",__func__);
		gpio_set_value(info->pdata->gpio_resetb, (mode >> cnt) & 1);
#endif
		udelay(3);
		isp_toggle_clk(info, 1, 0, 3);
	}
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Set Value 0 \n",__func__);
	gpio_set_value(info->pdata->gpio_resetb, 0);
#endif
	local_irq_restore(flags);
}

static void isp_exit_mode(struct mms_ts_info *info)
{
	int i;
	unsigned long flags;

	local_irq_save(flags);
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif
	udelay(3);

	for (i = 0; i < 10; i++)
		isp_toggle_clk(info, 1, 0, 3);
	local_irq_restore(flags);
}

static void flash_set_address(struct mms_ts_info *info, u16 addr)
{
	/* Only 13 bits of addr are valid.
	 * The addr is in bits 13:1 of cmd */
	isp_send_bits(info, (u32)(addr & 0x1fff) << 1, 18);
}

static void flash_erase(struct mms_ts_info *info)
{
	isp_enter_mode(info, ISP_MODE_FLASH_ERASE);

#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif
	gpio_direction_output(info->pdata->gpio_scl, 0);
	gpio_direction_output(info->pdata->gpio_sda, 1);

	/* 4 clock cycles with different timings for the erase to
	 * get processed, clk is already 0 from above */
	udelay(7);
	isp_toggle_clk(info, 1, 0, 3);
	udelay(7);
	isp_toggle_clk(info, 1, 0, 3);
	usleep_range(25000, 35000);
	isp_toggle_clk(info, 1, 0, 3);
	usleep_range(150, 200);
	isp_toggle_clk(info, 1, 0, 3);

	gpio_set_value(info->pdata->gpio_sda, 0);

	isp_exit_mode(info);
}

static u32 flash_readl(struct mms_ts_info *info, u16 addr)
{
	int i;
	u32 val;
	unsigned long flags;

	local_irq_save(flags);
	isp_enter_mode(info, ISP_MODE_FLASH_READ);
	flash_set_address(info, addr);

	gpio_direction_output(info->pdata->gpio_scl, 0);
	gpio_direction_output(info->pdata->gpio_sda, 0);
	udelay(40);

	/* data load cycle */
	for (i = 0; i < 6; i++)
		isp_toggle_clk(info, 1, 0, 10);

	val = isp_recv_bits(info, 32);
	isp_exit_mode(info);
	local_irq_restore(flags);

	return val;
}

static void flash_writel(struct mms_ts_info *info, u16 addr, u32 val)
{
	unsigned long flags;

	local_irq_save(flags);
	isp_enter_mode(info, ISP_MODE_FLASH_WRITE);
	flash_set_address(info, addr);
	isp_send_bits(info, val, 32);

	gpio_direction_output(info->pdata->gpio_sda, 1);
	/* 6 clock cycles with different timings for the data to get written
	 * into flash */
	isp_toggle_clk(info, 0, 1, 3);
	isp_toggle_clk(info, 0, 1, 3);
	isp_toggle_clk(info, 0, 1, 6);
	isp_toggle_clk(info, 0, 1, 12);
	isp_toggle_clk(info, 0, 1, 3);
	isp_toggle_clk(info, 0, 1, 3);

	isp_toggle_clk(info, 1, 0, 1);

	gpio_direction_output(info->pdata->gpio_sda, 0);
	isp_exit_mode(info);
	local_irq_restore(flags);
	usleep_range(300, 400);
}

static bool flash_is_erased(struct mms_ts_info *info)
{
	struct i2c_client *client = info->client;
	u32 val;
	u16 addr;

	for (addr = 0; addr < (ISP_MAX_FW_SIZE / 4); addr++) {
		udelay(40);
		val = flash_readl(info, addr);

		if (val != 0xffffffff) {
			dev_dbg(&client->dev,
				"addr 0x%x not erased: 0x%08x != 0xffffffff\n",
				addr, val);
			return false;
		}
	}
	return true;
}

static int fw_write_image(struct mms_ts_info *info, const u8 *data, size_t len)
{
	struct i2c_client *client = info->client;
	u16 addr = 0;

	for (addr = 0; addr < (len / 4); addr++, data += 4) {
		u32 val = get_unaligned_le32(data);
		u32 verify_val;
		int retries = 3;

		while (retries--) {
			flash_writel(info, addr, val);
			verify_val = flash_readl(info, addr);
			if (val == verify_val)
				break;
			dev_err(&client->dev,
				"mismatch @ addr 0x%x: 0x%x != 0x%x\n",
				addr, verify_val, val);
			continue;
		}
		if (retries < 0)
			return -ENXIO;
	}

	return 0;
}

static int fw_download(struct mms_ts_info *info, const u8 *data, size_t len)
{
	struct i2c_client *client = info->client;
	u32 val;
	int ret = 0;

	if (len % 4) {
		dev_err(&client->dev,
			"fw image size (%d) must be a multiple of 4 bytes\n",
			len);
		return -EINVAL;
	} else if (len > ISP_MAX_FW_SIZE) {
		dev_err(&client->dev,
			"fw image is too big, %d > %d\n", len, ISP_MAX_FW_SIZE);
		return -EINVAL;
	}

	dev_info(&client->dev, "fw download start\n");

	melfas_vdd_on(info, 0);
	gpio_direction_output(info->pdata->gpio_sda, 0);
	gpio_direction_output(info->pdata->gpio_scl, 0);
#ifdef MMS_144_RESET_PIN
	printk("[TSP] %s : Reset pin Output value 0 \n",__func__);
	gpio_direction_output(info->pdata->gpio_resetb, 0);
#endif

	hw_reboot_bootloader(info);

	val = flash_readl(info, ISP_IC_INFO_ADDR);
	dev_info(&client->dev, "IC info: 0x%02x (%x)\n", val & 0xff, val);

	dev_info(&client->dev, "fw erase...\n");
	flash_erase(info);
	if (!flash_is_erased(info)) {
		ret = -ENXIO;
		goto err;
	}

	dev_info(&client->dev, "fw write...\n");
	/* XXX: what does this do?! */
	flash_writel(info, ISP_IC_INFO_ADDR, 0xffffff00 | (val & 0xff));
	usleep_range(1000, 1500);
	ret = fw_write_image(info, data, len);
	if (ret)
		goto err;
	usleep_range(1000, 1500);

	hw_reboot_normal(info);
	usleep_range(1000, 1500);
	dev_info(&client->dev, "fw download done...\n");
	return 0;

err:
	dev_err(&client->dev, "fw download failed...\n");
	hw_reboot_normal(info);
	return ret;
}
#endif /* fw call removal */

#if defined(SEC_TSP_ISC_FW_UPDATE)
static u16 gen_crc(u8 data, u16 pre_crc)
{
	u16 crc;
	u16 cur;
	u16 temp;
	u16 bit_1;
	u16 bit_2;
	int i;

	crc = pre_crc;
	for (i = 7; i >= 0; i--) {
		cur = ((data >> i) & 0x01) ^ (crc & 0x0001);
		bit_1 = cur ^ (crc >> 11 & 0x01);
		bit_2 = cur ^ (crc >> 4 & 0x01);
		temp = (cur << 4) | (crc >> 12 & 0x0F);
		temp = (temp << 7) | (bit_1 << 6) | (crc >> 5 & 0x3F);
		temp = (temp << 4) | (bit_2 << 3) | (crc >> 1 & 0x0007);
		crc = temp;
	}
	return crc;
}

static int isc_fw_download(struct mms_ts_info *info, const u8 *data,
		size_t len)
{
	u8 *buff;
	u16 crc_buf;
	int src_idx;
	int dest_idx;
	int ret;
	int i, j;

	buff = kzalloc(ISC_PKT_SIZE, GFP_KERNEL);
	if (!buff) {
		dev_err(&info->client->dev, "%s: failed to allocate memory\n",
			__func__);
		ret = -1;
		goto err_mem_alloc;
	}

	/* enterring ISC mode */
	*buff = ISC_ENTER_ISC_DATA;
	ret = i2c_smbus_write_byte_data(info->client,
		ISC_ENTER_ISC_CMD, *buff);
	if (ret < 0) {
		dev_err(&info->client->dev,
			"fail to enter ISC mode(err=%d)\n", ret);
		goto fail_to_isc_enter;
	}
	usleep_range(10000, 20000);
	dev_info(&info->client->dev, "Enter ISC mode\n");

	/*enter ISC update mode */
	*buff = ISC_ENTER_UPDATE_DATA;
	ret = i2c_smbus_write_i2c_block_data(info->client,
		ISC_CMD,
		ISC_ENTER_UPDATE_DATA_LEN, buff);
	if (ret < 0) {
		dev_err(&info->client->dev,
			"fail to enter ISC update mode(err=%d)\n", ret);
		goto fail_to_isc_update;
	}
	dev_info(&info->client->dev, "Enter ISC update mode\n");

	/* firmware write */
	*buff = ISC_CMD;
	*(buff + 1) = ISC_DATA_WRITE_SUB_CMD;
	for (i = 0; i < ISC_PKT_NUM; i++) {
		*(buff + 2) = i;
		crc_buf = gen_crc(*(buff + 2), ISC_DEFAULT_CRC);

		for (j = 0; j < ISC_PKT_DATA_SIZE; j++) {
			dest_idx = ISC_PKT_HEADER_SIZE + j;
			src_idx =  i * ISC_PKT_DATA_SIZE +
				((int)(j / WORD_SIZE)) * WORD_SIZE -
				(j % WORD_SIZE) + 3;
			*(buff + dest_idx) = *(data + src_idx);
			crc_buf = gen_crc(*(buff + dest_idx), crc_buf);
		}

		*(buff + ISC_PKT_DATA_SIZE + ISC_PKT_HEADER_SIZE + 1) =
			crc_buf & 0xFF;
		*(buff + ISC_PKT_DATA_SIZE + ISC_PKT_HEADER_SIZE) =
			crc_buf >> 8 & 0xFF;

		ret = i2c_master_send(info->client, buff, ISC_PKT_SIZE);
		if (ret < 0) {
			dev_err(&info->client->dev,
				"fail to firmware writing on packet %d.(%d)\n",
				i, ret);
			goto fail_to_fw_write;
		}
		usleep_range(1, 5);

		/* confirm CRC */
		ret = i2c_smbus_read_byte_data(info->client,
			ISC_CHECK_STATUS_CMD);
		if (ret == ISC_CONFIRM_CRC) {
			dev_info(&info->client->dev,
				"updating %dth firmware data packet.\n", i);
		} else {
			dev_err(&info->client->dev,
				"fail to firmware update on %dth (%X).\n",
				i, ret);
			ret = -1;
			goto fail_to_confirm_crc;
		}
	}

	ret = 0;

fail_to_confirm_crc:
fail_to_fw_write:
	/* exit ISC mode */
	*buff = ISC_EXIT_ISC_SUB_CMD;
	*(buff + 1) = ISC_EXIT_ISC_SUB_CMD2;
	i2c_smbus_write_i2c_block_data(info->client, ISC_CMD, 2, buff);
	usleep_range(10000, 20000);
fail_to_isc_update:
	hw_reboot_normal(info);
fail_to_isc_enter:
	kfree(buff);
err_mem_alloc:
	return ret;
}
#endif /* SEC_TSP_ISC_FW_UPDATE */

static int get_fw_version(struct mms_ts_info *info)
{
	int ret;
	int retries = 3;

	/* this seems to fail sometimes after a reset.. retry a few times */
	do {
		ret = i2c_smbus_read_byte_data(info->client, MMS_FW_VERSION);
		if(retries==1)
		{
		mms_pwr_on_reset(info);
		msleep(30);
		}
	} while (ret < 0 && retries-- > 0);

	return ret;
}

static int get_hw_version(struct mms_ts_info *info)
{
	int ret;
	int retries = 3;

	/* this seems to fail sometimes after a reset.. retry a few times */
	do {
		ret = i2c_smbus_read_byte_data(info->client, MMS_HW_REVISION);
	} while (ret < 0 && retries-- > 0);

	return ret;
}

static int mms_ts_enable(struct mms_ts_info *info, int wakeupcmd)
{
	//mutex_lock(&info->lock);
	if (info->enabled)
		goto out;
	/* wake up the touch controller. */
	if (wakeupcmd == 1) {
		i2c_smbus_write_byte_data(info->client, 0, 0);
		usleep_range(3000, 5000);
	}
	info->enabled = true;
	enable_irq(info->irq);
out:
	//mutex_unlock(&info->lock);
	return 0;
}

static int mms_ts_disable(struct mms_ts_info *info, int sleepcmd)
{
	//mutex_lock(&info->lock);
	if (!info->enabled)
		goto out;
	disable_irq(info->irq);
	if (sleepcmd == 1) {
		i2c_smbus_write_byte_data(info->client, MMS_MODE_CONTROL, 0);
		usleep_range(10000, 12000);
	}
	info->enabled = false;
	touch_is_pressed = 0;
out:
	//mutex_unlock(&info->lock);
	return 0;
}

#if 0
static int mms_ts_finish_config(struct mms_ts_info *info)
{
	struct i2c_client *client = info->client;
	int ret;

	ret = request_threaded_irq(client->irq, NULL, mms_ts_interrupt,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				   "mms_ts", info);
	if (ret < 0) {
		ret = 1;
		dev_err(&client->dev, "Failed to register interrupt\n");
		goto err_req_irq;
	}

	info->irq = client->irq;
	barrier();

	dev_info(&client->dev,
		 "Melfas MMS-series touch controller initialized\n");

	return 0;

err_req_irq:
	return ret;
}
#endif

#if !ISC_DL_MODE
static int mms_ts_fw_info(struct mms_ts_info *info)
{
	struct i2c_client *client = info->client;
	int ret = 0;
	int ver, hw_rev;

	ver = get_fw_version(info);
	info->fw_ic_ver = ver;
		dev_info(&client->dev,
			 "[TSP]fw version 0x%02x !!!!\n", ver);

	hw_rev = get_hw_version(info);
	dev_info(&client->dev,
		"[TSP] hw rev = %x\n", hw_rev);

	if (ver < 0 || hw_rev < 0) {
		ret = 1;
		dev_err(&client->dev,
			"i2c fail...tsp driver unload.\n");
		return ret;
	}
/*
	if (!info->pdata || !info->pdata->mux_fw_flash) {
		ret = 1;
		dev_err(&client->dev,
			"fw cannot be updated, missing platform data\n");
		return ret;
	}
*/
//	ret = mms_ts_finish_config(info);

	return ret;
}
#endif

#if ISC_DL_MODE
static int mms_ts_fw_load(struct mms_ts_info *info)
{

	struct i2c_client *client = info->client;
	/*struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);*/
	int ret = 0;
	int ver, hw_rev;
	/*int retries = 3;*/

	ver = get_fw_version(info);
	info->fw_ic_ver = ver;
		dev_info(&client->dev,
			 "[TSP]fw version 0x%02x !!!!\n", ver);

	hw_rev = get_hw_version(info);
	dev_info(&client->dev,
		"[TSP]hw rev = 0x%02x\n", hw_rev);

	if (ver < 0 || hw_rev < 0) {
		ret = 1;
		dev_err(&client->dev,
			"i2c fail...tsp driver unload.\n");
	}
/*
	if (!info->pdata || !info->pdata->mux_fw_flash) {
		ret = 1;
		dev_err(&client->dev,
			"fw cannot be updated, missing platform data\n");
		goto out;
	}
*/
	if (hw_rev == 0xC) {
		dev_err(&client->dev,
			"[TSP] support only 4.8 inch panel. Do not update");
		goto done;
	}

	dev_err(&client->dev,
		"[TSP] ISC Ver [0x%02x] [0x%02x] [0x%02x]",
		i2c_smbus_read_byte_data(info->client, 0xF3),
		i2c_smbus_read_byte_data(info->client, 0xF4),
		i2c_smbus_read_byte_data(info->client, 0xF5));

#if 0
	if (ver >= FW_VERSION && ver != 0xFF
			&& ver != 0x00 && ver != 0x45) {
		dev_err(&client->dev,
			"[TSP] fw is latest. Do not update.");
		goto done;
	} else {
		dev_err(&client->dev,
			"[TSP] fw update [0x%02x -> 0x%02x]",
			ver, FW_VERSION);
	}
#endif
	dev_err(&client->dev,
			"[TSP] fw update [0x%02x -> 0x%02x]",
			ver, FW_VERSION);


	ret = mms100_ISC_download_mbinary(info);

	if (ret == 0) {
		dev_err(&client->dev,
			"[TSP] mms100_ISC_download_mbinary success");
		goto done;
	} else {
		dev_err(&client->dev,
			"[TSP] mms100_ISC_download_mbinary fail [%d]", ret);
		ret = 1;
	}
//out:
	return ret;

done:
//	ret = mms_ts_finish_config(info);
	return ret;
}
#endif

#ifdef SEC_TSP_FACTORY_TEST
static void set_default_result(struct mms_ts_info *info)
{
	char delim = ':';

	memset(info->cmd_result, 0x00, ARRAY_SIZE(info->cmd_result));
	memcpy(info->cmd_result, info->cmd, strlen(info->cmd));
	strncat(info->cmd_result, &delim, 1);
}

static void set_cmd_result(struct mms_ts_info *info, char *buff, int len)
{
	strncat(info->cmd_result, buff, len);
}

/*
static inline int msm_irq_to_gpio(unsigned irq)
{
	// TODO : Need to verify chip->base=0
	return irq - MSM_GPIO_TO_INT(0);
}
*/
static void get_raw_data_all(struct mms_ts_info *info, u8 cmd)
{
	u8 w_buf[6];
	u8 read_buffer[2]; /* 52 */
	char buff[TSP_CMD_STR_LEN] = {0};
	int gpio;
	int ret;
	int i, j;
	u32 max_value, min_value;
	u32 raw_data;
	gpio = info->pdata->gpio_int;
//	gpio = msm_irq_to_gpio(info->irq);
	disable_irq(info->irq);

	w_buf[0] = MMS_VSC_CMD;		/* vendor specific command id */
	w_buf[1] = MMS_VSC_MODE;	/* mode of vendor */
	w_buf[2] = 0;			/* tx line */
	w_buf[3] = 0;			/* rx line */
	w_buf[4] = 0;			/* reserved */
	w_buf[5] = 0;			/* sub command */

	if (cmd == MMS_VSC_CMD_EXIT) {
		w_buf[5] = MMS_VSC_CMD_EXIT; /* exit test mode */

		ret = i2c_smbus_write_i2c_block_data(info->client,
			w_buf[0], 5, &w_buf[1]);
		if (ret < 0)
			goto err_i2c;
		touch_is_pressed = 0;
		release_all_fingers(info);
#if defined(CONFIG_MIPI_SAMSUNG_ESD_REFRESH)
		set_esd_disable();
#endif
		melfas_vdd_on(info, 0);
		msleep(30);
		melfas_vdd_on(info, 1);
		msleep(120);
		enable_irq(info->irq);
#if defined(CONFIG_MIPI_SAMSUNG_ESD_REFRESH)
		set_esd_enable();
#endif
		return ;
	}

	/* MMS_VSC_CMD_CM_DELTA or MMS_VSC_CMD_CM_ABS
	 * this two mode need to enter the test mode
	 * exit command must be followed by testing.
	 */
	if (cmd == MMS_VSC_CMD_CM_DELTA || cmd == MMS_VSC_CMD_CM_ABS) {
		/* enter the debug mode */
		w_buf[2] = 0x0; /* tx */
		w_buf[3] = 0x0; /* rx */
		w_buf[5] = MMS_VSC_CMD_ENTER;

		ret = i2c_smbus_write_i2c_block_data(info->client,
			w_buf[0], 5, &w_buf[1]);
		if (ret < 0)
			goto err_i2c;

		/* wating for the interrupt */
		while (gpio_get_value(gpio))
			udelay(100);
	}

	max_value = 0;
	min_value = 0;

	for (i = 0; i < RX_NUM; i++) {
		for (j = 0; j < TX_NUM; j++) {

			w_buf[2] = j; /* tx */
			w_buf[3] = i; /* rx */
			w_buf[5] = cmd;

			ret = i2c_smbus_write_i2c_block_data(info->client,
					w_buf[0], 5, &w_buf[1]);
			if (ret < 0)
				goto err_i2c;

			usleep_range(1, 5);

			ret = i2c_smbus_read_i2c_block_data(info->client, 0xBF,
					2, read_buffer);
			if (ret < 0)
				goto err_i2c;

			raw_data = ((u16)read_buffer[1] << 8) | read_buffer[0];

			if (i == 0 && j == 0) {
				max_value = min_value = raw_data;
			} else {
				max_value = max(max_value, raw_data);
				min_value = min(min_value, raw_data);
			}

			if (cmd == MMS_VSC_CMD_INTENSITY) {
				info->intensity[j * RX_NUM + i] = raw_data;
				dev_dbg(&info->client->dev, "[TSP] intensity[%d][%d] = %d\n",
					i, j, info->intensity[j * RX_NUM + i]);
			} else if (cmd == MMS_VSC_CMD_CM_DELTA) {
				info->inspection[j * RX_NUM + i] = raw_data;
				dev_dbg(&info->client->dev, "[TSP] delta[%d][%d] = %d\n",
					i, j, info->inspection[j * RX_NUM + i]);
			} else if (cmd == MMS_VSC_CMD_CM_ABS) {
				info->raw[j * RX_NUM + i] = raw_data;
				dev_dbg(&info->client->dev, "[TSP] raw[%d][%d] = %d\n",
					i, j, info->raw[j * RX_NUM + i]);
			} else if (cmd == MMS_VSC_CMD_REFER) {
				info->reference[j * RX_NUM + i] =
						raw_data >> 3;
				dev_dbg(&info->client->dev, "[TSP] reference[%d][%d] = %d\n",
					i, j, info->reference[j * RX_NUM + i]);
			}
		}
	}

	snprintf(buff, sizeof(buff), "%d,%d", min_value, max_value);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));

	enable_irq(info->irq);

err_i2c:
	dev_err(&info->client->dev, "%s: fail to i2c (cmd=%d)\n",
			__func__, cmd);
}

#ifdef ESD_DEBUG
static u32 get_raw_data_one(struct mms_ts_info *info, u16 rx_idx, u16 tx_idx,
		u8 cmd)
{
	u8 w_buf[6];
	u8 read_buffer[2];
	int ret;
	u32 raw_data;

	w_buf[0] = MMS_VSC_CMD;		/* vendor specific command id */
	w_buf[1] = MMS_VSC_MODE;	/* mode of vendor */
	w_buf[2] = 0;			/* tx line */
	w_buf[3] = 0;			/* rx line */
	w_buf[4] = 0;			/* reserved */
	w_buf[5] = 0;			/* sub command */

	if (cmd != MMS_VSC_CMD_INTENSITY && cmd != MMS_VSC_CMD_RAW &&
				cmd != MMS_VSC_CMD_REFER) {
		dev_err(&info->client->dev, "%s: not profer command(cmd=%d)\n",
				__func__, cmd);
		return FAIL;
	}

	w_buf[2] = tx_idx;	/* tx */
	w_buf[3] = rx_idx;	/* rx */
	w_buf[5] = cmd;		/* sub command */

	ret = i2c_smbus_write_i2c_block_data(info->client, w_buf[0], 5,
			&w_buf[1]);
	if (ret < 0)
		goto err_i2c;

	ret = i2c_smbus_read_i2c_block_data(info->client, 0xBF, 2,
			read_buffer);
	if (ret < 0)
		goto err_i2c;

	raw_data = ((u16)read_buffer[1] << 8) | read_buffer[0];
	if (cmd == MMS_VSC_CMD_REFER)
		raw_data = raw_data >> 4;

	return raw_data;

err_i2c:
	dev_err(&info->client->dev, "%s: fail to i2c (cmd=%d)\n",
			__func__, cmd);
	return FAIL;
}
#endif

#if 0 /* sysfs function call */
static ssize_t show_close_tsp_test(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mms_ts_info *info = dev_get_drvdata(dev);

	get_raw_data_all(info, MMS_VSC_CMD_EXIT);
	info->ft_flag = 0;

	return snprintf(buf, TSP_BUF_SIZE, "%u\n", 0);
}
#endif /* sysfs function call */

static int check_rx_tx_num(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[TSP_CMD_STR_LEN] = {0};
	int node;

	if (info->cmd_param[0] < 0 ||
			info->cmd_param[0] >= TX_NUM  ||
			info->cmd_param[1] < 0 ||
			info->cmd_param[1] >= RX_NUM) {
		snprintf(buff, sizeof(buff) , "%s", "NG");
		set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
		info->cmd_state = 3;

		dev_info(&info->client->dev, "%s: parameter error: %u,%u\n",
				__func__, info->cmd_param[0],
				info->cmd_param[1]);
		node = -1;
		return node;
	}
	node = info->cmd_param[0] * RX_NUM + info->cmd_param[1];
	dev_info(&info->client->dev, "%s: node = %d\n", __func__,
			node);
	return node;

}

static void cover_set(struct mms_ts_info *info, int state){
	if(state == COVER_OPEN){
		dev_info(&info->client->dev, "[TSP] %s: opened\n", __func__);
		i2c_smbus_write_byte_data(info->client, 0x34, 0x0);

	} else if(state == COVER_CLOSED) {
		dev_info(&info->client->dev, "[TSP] %s: closed\n", __func__);
		i2c_smbus_write_byte_data(info->client, 0x34, 0x1);
	}
}

static void clear_cover_mode(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;
	char buff[TSP_CMD_STR_LEN] = {0};
	int arg = info->cmd_param[0];

	set_default_result(info);
	snprintf(buff, sizeof(buff), "%u", arg);

	dev_info(&info->client->dev, "[TSP] %s: arg=%d\n", __func__,arg);

	info->cover_state = arg;

	if(info->enabled)
		cover_set(info, arg);

	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));

	info->cmd_is_running = false;

	info->cmd_state = 2;

	return;
}

static void not_support_cmd(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;
	char buff[16] = {0};

	set_default_result(info);
	sprintf(buff, "%s", "NA");
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 4;
	dev_info(&info->client->dev, "%s: \"%s(%d)\"\n", __func__,
				buff, (int)strnlen(buff, sizeof(buff)));
	return;
}

static void fw_update(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;
	struct i2c_client *client = info->client;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	int ret = 0;
	int fw_ver = 0, ver = 0, hw_rev = 0, fw_bin_ver = 0;
	int retries = 5;
	const u8 *buff = 0;
	mm_segment_t old_fs = {0};
	struct file *fp = NULL;
	long fsize = 0, nread = 0;
	char result_buff[16] = {0};

	#define MMS_TS "/sdcard/melfas_fw.bin"

	set_default_result(info);

	hw_rev = get_hw_version(info);
	if (hw_rev == 0x1 || hw_rev == 0x32)
		fw_bin_ver = FW_VERSION;
	else if (hw_rev == 0x0C)
		fw_bin_ver = FW_465_VERSION;

	fw_ver = get_fw_version(info);
	dev_info(&client->dev,
		"fw_ic_ver = 0x%02x, fw_bin_ver = 0x%02x\n",
		fw_ver, fw_bin_ver);

	if (info->cmd_param[0] == 0
			&& fw_ver >= fw_bin_ver) {
		dev_info(&client->dev,
			"fw version update does not need\n");
		goto do_not_need_update;
	}

	ret=mms_ts_fw_load(info);
	if (ret){
		snprintf(result_buff, sizeof(result_buff), "FAIL");
		set_cmd_result(info, result_buff, strnlen(result_buff, sizeof(result_buff)));
		info->cmd_state = 3;
		return;
	}
	else {
		snprintf(result_buff, sizeof(result_buff), "OK");
		set_cmd_result(info, result_buff, strnlen(result_buff, sizeof(result_buff)));
		info->cmd_state = 2;
		return;
	}
	switch (info->cmd_param[0]) {
	case BUILT_IN:
		if (hw_rev == 0x1 || hw_rev == 0x32) {
			buff = MELFAS_binary;
			fsize = MELFAS_binary_nLength;
			dev_info(&client->dev, "built in 4.8 fw is loaded!!\n");
		} else if (hw_rev == 0x0C) {
			buff = MELFAS_465_binary;
			fsize = MELFAS_465_binary_nLength;
			dev_info(&client->dev, "built in 4.65 fw is loaded!!\n");
		}
		break;

	case UMS:
		old_fs = get_fs();
		set_fs(get_ds());

		fp = filp_open(MMS_TS, O_RDONLY, 0);
		if (IS_ERR(fp)) {
			dev_err(&client->dev,
				"file %s open error\n", MMS_TS);
			info->cmd_state = 3;
			goto err_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;

		buff = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!buff) {
			dev_err(&client->dev, "fail to alloc buffer for fw\n");
			info->cmd_state = 3;
			goto err_alloc;
		}

		nread = vfs_read(fp, (char __user *)buff, fsize, &fp->f_pos);
		if (nread != fsize) {
			dev_err(&client->dev, "fail to read file %s (nread = %ld)\n",
					MMS_TS, nread);
			info->cmd_state = 3;
			goto err_fw_size;
		}

		filp_close(fp, current->files);
		set_fs(old_fs);
		dev_info(&client->dev, "ums fw is loaded!!\n");
		break;

	default:
		dev_err(&client->dev, "invalid fw file type!!\n");
		goto not_support;
	}

	disable_irq(info->irq);
	while (retries--) {
		i2c_lock_adapter(adapter);
#if 0 /* firmware call removal */
		melfas_mux_fw_flash(info->pdata, true);

		ret = fw_download(info, (const u8 *)buff,
				(const size_t)fsize);

		melfas_mux_fw_flash(info->pdata, false);
#endif /* firmware call removal */
		i2c_unlock_adapter(adapter);

		if (ret < 0) {
			dev_err(&client->dev, "retrying flashing\n");
			continue;
		}

		ver = get_fw_version(info);
		info->fw_ic_ver = ver;

		if (info->cmd_param[0] == 1) {
			dev_info(&client->dev,
			  "fw update done. ver = 0x%02x\n", ver);
			info->cmd_state = 2;
			enable_irq(info->irq);
			return;
		} else if (ver == fw_bin_ver) {
			dev_info(&client->dev,
			  "fw update done. ver = 0x%02x\n", ver);
			info->cmd_state = 2;
			enable_irq(info->irq);
			return;
		} else {
			dev_err(&client->dev,
					"ERROR : fw version is still wrong (0x%x != 0x%x)\n",
					ver, FW_VERSION);
		}
		dev_err(&client->dev, "retrying flashing\n");
	}

err_fw_size:
	kfree(buff);
err_alloc:
	filp_close(fp, NULL);
err_open:
	set_fs(old_fs);
not_support:
do_not_need_update:
	info->cmd_state = 2;
	return;
}

static void get_fw_ver_bin(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	int hw_rev;

	set_default_result(info);
	hw_rev = get_hw_version(info);
	if (hw_rev == 0x01)
		snprintf(buff, sizeof(buff), "ME0000%x", FW_VERSION);
	else
		snprintf(buff, sizeof(buff), "ME0000%x", FW_465_VERSION);

	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;
	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));
}

static void get_fw_ver_ic(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	int ver;

	set_default_result(info);

	ver = get_fw_version(info);

	snprintf(buff, sizeof(buff), "ME0000%x", ver);

	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;
	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));
}

static void get_config_ver(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[20] = {0};

	set_default_result(info);

//	snprintf(buff, sizeof(buff), "%s", info->config_fw_version);
	snprintf(buff, sizeof(buff), "N/A");
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;
	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));
}

static void get_threshold(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	int threshold;

	set_default_result(info);

	threshold = i2c_smbus_read_byte_data(info->client, 0x05);
	if (threshold < 0) {
		snprintf(buff, sizeof(buff), "%s", "NG");
		set_cmd_result(info, buff, (int)strnlen(buff, sizeof(buff)));
		info->cmd_state = 3;
		return;
	}
	snprintf(buff, sizeof(buff), "%d", threshold);

	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;
	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));
}

static void module_off_master(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[3] = {0};

	//mutex_lock(&info->lock);
	if (info->enabled) {
		disable_irq(info->irq);
		info->enabled = false;
		touch_is_pressed = 0;
	}
	//mutex_unlock(&info->lock);

	melfas_vdd_on(info, 0);

	if (is_melfas_vdd_on(info) == 0)
		snprintf(buff, sizeof(buff), "%s", "OK");
	else
		snprintf(buff, sizeof(buff), "%s", "NG");

	set_default_result(info);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));

	if (strncmp(buff, "OK", 2) == 0)
		info->cmd_state = 2;
	else
		info->cmd_state = 3;

	dev_info(&info->client->dev, "%s: %s\n", __func__, buff);
}

static void module_on_master(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[3] = {0};

	mms_pwr_on_reset(info);

	//mutex_lock(&info->lock);
	if (!info->enabled) {
		enable_irq(info->irq);
		info->enabled = true;
	}
	//mutex_unlock(&info->lock);

	if (is_melfas_vdd_on(info) == 1)
		snprintf(buff, sizeof(buff), "%s", "OK");
	else
		snprintf(buff, sizeof(buff), "%s", "NG");

	set_default_result(info);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));

	if (strncmp(buff, "OK", 2) == 0)
		info->cmd_state = 2;
	else
		info->cmd_state = 3;

	dev_info(&info->client->dev, "%s: %s\n", __func__, buff);

}

static void get_chip_vendor(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};

	set_default_result(info);

	snprintf(buff, sizeof(buff), "%s", "MELFAS");
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;
	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));
}

static void get_chip_name(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};

	set_default_result(info);

	snprintf(buff, sizeof(buff), "%s", "MMS144");
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;
	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));
}

static void get_reference(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	unsigned int val;
	int node;

	set_default_result(info);
	node = check_rx_tx_num(info);

	if (node < 0)
		return;

	val = info->reference[node];
	snprintf(buff, sizeof(buff), "%u", val);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));

	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__,
			buff, (int)strnlen(buff, sizeof(buff)));

}

static void get_cm_abs(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	unsigned int val;
	int node;

	set_default_result(info);
	node = check_rx_tx_num(info);

	if (node < 0)
		return;

	val = info->raw[node];
	snprintf(buff, sizeof(buff), "%u", val);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__, buff,
			(int)strnlen(buff, sizeof(buff)));
}

static void get_cm_delta(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	unsigned int val;
	int node;

	set_default_result(info);
	node = check_rx_tx_num(info);

	if (node < 0)
		return;

	val = info->inspection[node];
	snprintf(buff, sizeof(buff), "%u", val);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__, buff,
			(int)strnlen(buff, sizeof(buff)));
}

static void get_intensity(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	unsigned int val;
	int node;

	set_default_result(info);
	node = check_rx_tx_num(info);

	if (node < 0)
		return;

	val = info->intensity[node];

	snprintf(buff, sizeof(buff), "%u", val);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__, buff,
			(int)strnlen(buff, sizeof(buff)));
}

static void get_x_num(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	int val;

	set_default_result(info);
	val = i2c_smbus_read_byte_data(info->client, 0xEF);
	if (val < 0) {
		snprintf(buff, sizeof(buff), "%s", "NG");
		set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
		info->cmd_state = 3;

		dev_info(&info->client->dev,
			"%s: fail to read num of x (%d).\n", __func__, val);

		return ;
	}
	snprintf(buff, sizeof(buff), "%u", val);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__, buff,
			(int)strnlen(buff, sizeof(buff)));
}

static void get_y_num(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	char buff[16] = {0};
	int val;

	set_default_result(info);
	val = i2c_smbus_read_byte_data(info->client, 0xEE);
	if (val < 0) {
		snprintf(buff, sizeof(buff), "%s", "NG");
		set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
		info->cmd_state = 3;

		dev_info(&info->client->dev,
			"%s: fail to read num of y (%d).\n", __func__, val);

		return ;
	}
	snprintf(buff, sizeof(buff), "%u", val);
	set_cmd_result(info, buff, strnlen(buff, sizeof(buff)));
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s: %s(%d)\n", __func__, buff,
			(int)strnlen(buff, sizeof(buff)));
}

static void run_reference_read(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	set_default_result(info);
	get_raw_data_all(info, MMS_VSC_CMD_REFER);
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s\n", __func__);
}

static void run_cm_abs_read(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	set_default_result(info);
	get_raw_data_all(info, MMS_VSC_CMD_CM_ABS);
	get_raw_data_all(info, MMS_VSC_CMD_EXIT);
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s\n", __func__);
}

static void run_cm_delta_read(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	set_default_result(info);
	get_raw_data_all(info, MMS_VSC_CMD_CM_DELTA);
	get_raw_data_all(info, MMS_VSC_CMD_EXIT);
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s\n", __func__);
}

static void run_intensity_read(void *device_data)
{
	struct mms_ts_info *info = (struct mms_ts_info *)device_data;

	set_default_result(info);
	get_raw_data_all(info, MMS_VSC_CMD_INTENSITY);
	info->cmd_state = 2;

	dev_info(&info->client->dev, "%s\n", __func__);
}

#if 0 /* sysfs device function */
static ssize_t store_cmd(struct device *dev, struct device_attribute
		*devattr, const char *buf, size_t count)
{
	struct mms_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;

	char *cur, *start, *end;
	char buff[TSP_CMD_STR_LEN] = {0};
	int len, i;
	struct tsp_cmd *tsp_cmd_ptr = NULL;
	char delim = ',';
	bool cmd_found = false;
	int param_cnt = 0;


	if (info->cmd_is_running == true) {
		dev_err(&info->client->dev, "tsp_cmd: other cmd is running.\n");
		goto err_out;
	}


	/* check lock  */
	mutex_lock(&info->cmd_lock);
	info->cmd_is_running = true;
	mutex_unlock(&info->cmd_lock);

	info->cmd_state = 1;

	for (i = 0; i < ARRAY_SIZE(info->cmd_param); i++)
		info->cmd_param[i] = 0;

	len = (int)count;
	if (*(buf + len - 1) == '\n')
		len--;
	memset(info->cmd, 0x00, ARRAY_SIZE(info->cmd));
	memcpy(info->cmd, buf, len);

	cur = strchr(buf, (int)delim);
	if (cur)
		memcpy(buff, buf, cur - buf);
	else
		memcpy(buff, buf, len);

	/* find command */
	list_for_each_entry(tsp_cmd_ptr, &info->cmd_list_head, list) {
		if (!strcmp(buff, tsp_cmd_ptr->cmd_name)) {
			cmd_found = true;
			break;
		}
	}

	/* set not_support_cmd */
	if (!cmd_found) {
		list_for_each_entry(tsp_cmd_ptr, &info->cmd_list_head, list) {
			if (!strcmp("not_support_cmd", tsp_cmd_ptr->cmd_name))
				break;
		}
	}

	/* parsing parameters */
	if (cur && cmd_found) {
		cur++;
		start = cur;
		memset(buff, 0x00, ARRAY_SIZE(buff));
		do {
			if (*cur == delim || cur - buf == len) {
				end = cur;
				memcpy(buff, start, end - start);
				*(buff + strlen(buff)) = '\0';
				if (kstrtoint(buff, 10,
					info->cmd_param + param_cnt) < 0)
					goto err_out;
				start = cur + 1;
				memset(buff, 0x00, ARRAY_SIZE(buff));
				param_cnt++;
			}
			cur++;
		} while (cur - buf <= len);
	}

	dev_info(&client->dev, "cmd = %s\n", tsp_cmd_ptr->cmd_name);
	for (i = 0; i < param_cnt; i++)
		dev_info(&client->dev, "cmd param %d= %d\n", i,
							info->cmd_param[i]);

	tsp_cmd_ptr->cmd_func(info);


err_out:
	return count;
}

static ssize_t show_cmd_status(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct mms_ts_info *info = dev_get_drvdata(dev);
	char buff[16] = {0};

	dev_info(&info->client->dev, "tsp cmd: status:%d\n",
			info->cmd_state);

	if (info->cmd_state == 0)
		snprintf(buff, sizeof(buff), "WAITING");

	else if (info->cmd_state == 1)
		snprintf(buff, sizeof(buff), "RUNNING");

	else if (info->cmd_state == 2)
		snprintf(buff, sizeof(buff), "OK");

	else if (info->cmd_state == 3)
		snprintf(buff, sizeof(buff), "FAIL");

	else if (info->cmd_state == 4)
		snprintf(buff, sizeof(buff), "NOT_APPLICABLE");

	return snprintf(buf, TSP_BUF_SIZE, "%s\n", buff);
}

static ssize_t show_cmd_result(struct device *dev, struct device_attribute
		*devattr, char *buf)
{
	struct mms_ts_info *info = dev_get_drvdata(dev);

	dev_info(&info->client->dev, "tsp cmd: result: %s\n", info->cmd_result);

	mutex_lock(&info->cmd_lock);
	info->cmd_is_running = false;
	mutex_unlock(&info->cmd_lock);

	info->cmd_state = 0;

	return snprintf(buf, TSP_BUF_SIZE, "%s\n", info->cmd_result);
}
#endif /* sysfs device function */

#ifdef ESD_DEBUG

static bool intensity_log_flag;

static ssize_t show_intensity_logging_on(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct mms_ts_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	struct file *fp;
	char log_data[160] = {0,};
	char buff[16] = {0,};
	mm_segment_t old_fs;
	long nwrite;
	u32 val;
	int i, y, c;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

#define MELFAS_DEBUG_LOG_PATH "/sdcard/melfas_log"

	dev_info(&client->dev, "%s: start.\n", __func__);
	fp = filp_open(MELFAS_DEBUG_LOG_PATH, O_RDWR|O_CREAT,
			S_IRWXU|S_IRWXG|S_IRWXO);
	if (IS_ERR(fp)) {
		dev_err(&client->dev, "%s: fail to open log file\n", __func__);
		goto open_err;
	}

	intensity_log_flag = 1;
	do {
		for (y = 0; y < 3; y++) {
			/* for tx chanel 0~2 */
			memset(log_data, 0x00, 160);

			snprintf(buff, 16, "%1u: ", y);
			strncat(log_data, buff, strnlen(buff, 16));

			for (i = 0; i < RX_NUM; i++) {
				val = get_raw_data_one(info, i, y,
						MMS_VSC_CMD_INTENSITY);
				snprintf(buff, 16, "%5u, ", val);
				strncat(log_data, buff, strnlen(buff, 16));
			}
			memset(buff, '\n', 2);
			c = (y == 2) ? 2 : 1;
			strncat(log_data, buff, c);
			nwrite = vfs_write(fp, (const char __user *)log_data,
					strnlen(log_data, 160), &fp->f_pos);
		}
		usleep_range(5000);
	} while (intensity_log_flag);

	filp_close(fp, current->files);
	set_fs(old_fs);

	return 0;

 open_err:
	set_fs(old_fs);
	return FAIL;
}

static ssize_t show_intensity_logging_off(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct mms_ts_info *info = dev_get_drvdata(dev);
	intensity_log_flag = 0;
	usleep_range(10000);
	get_raw_data_all(info, MMS_VSC_CMD_EXIT);
	return 0;
}

#endif

#if 0 /* ignore sysfs device creation, for now */
static DEVICE_ATTR(close_tsp_test, S_IRUGO, show_close_tsp_test, NULL);
static DEVICE_ATTR(cmd, S_IWUSR | S_IWGRP, NULL, store_cmd);
static DEVICE_ATTR(cmd_status, S_IRUGO, show_cmd_status, NULL);
static DEVICE_ATTR(cmd_result, S_IRUGO, show_cmd_result, NULL);
#ifdef ESD_DEBUG
static DEVICE_ATTR(intensity_logging_on, S_IRUGO, show_intensity_logging_on,
		NULL);
static DEVICE_ATTR(intensity_logging_off, S_IRUGO, show_intensity_logging_off,
		NULL);
#endif

static struct attribute *sec_touch_facotry_attributes[] = {
		&dev_attr_close_tsp_test.attr,
		&dev_attr_cmd.attr,
		&dev_attr_cmd_status.attr,
		&dev_attr_cmd_result.attr,
#ifdef ESD_DEBUG
		&dev_attr_intensity_logging_on.attr,
		&dev_attr_intensity_logging_off.attr,
#endif
		NULL,
};

static struct attribute_group sec_touch_factory_attr_group = {
	.attrs = sec_touch_facotry_attributes,
};
#endif /* ignore sysfs device creation, for now */
#endif /* SEC_TSP_FACTORY_TEST */

#ifdef CONFIG_OF
static void melfas_request_gpio(struct mms_ts_platform_data *pdata)
{
	int ret;
	pr_info("[TSP] request gpio\n");

	ret = gpio_request(pdata->gpio_scl, "melfas_tsp_scl");
	if (ret) {
		pr_err("[TSP]%s: unable to request melfas_tsp_scl [%d]\n",
				__func__, pdata->gpio_scl);
		return;
	}

	ret = gpio_request(pdata->gpio_sda, "melfas_tsp_sda");
	if (ret) {
		pr_err("[TSP]%s: unable to request melfas_tsp_sda [%d]\n",
				__func__, pdata->gpio_sda);
		return;
	}

	ret = gpio_request(pdata->gpio_int, "melfas_tsp_irq");
	if (ret) {
		pr_err("[TSP]%s: unable to request melfas_tsp_irq [%d]\n",
				__func__, pdata->gpio_int);
		return;
	}

	/*
	pr_err("Requesting VDD GPIO : %d\n", pdata->vdd_en);
	ret = gpio_request(pdata->vdd_en, "melfas_vdd_en");
	if (ret) {
		pr_err("[TSP]%s: unable to request melfas_vdd_en [%d]\n",
				__func__, pdata->vdd_en);
		return;
	}
	*/
}



static int mms_get_dt_coords(struct device *dev, char *name,
				struct mms_ts_platform_data *pdata)
{
	u32 coords[MMS_COORDS_ARR_SIZE];
	struct property *prop;
	struct device_node *np = dev->of_node;
	int coords_size, rc;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	coords_size = prop->length / sizeof(u32);
	if (coords_size != MMS_COORDS_ARR_SIZE) {
		dev_err(dev, "invalid %s\n", name);
		return -EINVAL;
	}

	rc = of_property_read_u32_array(np, name, coords, coords_size);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read %s\n", name);
		return rc;
	}

	if (strncmp(name, "melfas,panel-coords",
			sizeof("melfas,panel-coords")) == 0) {
		pdata->invert_x = coords[0];
		pdata->invert_y = coords[1];
		pdata->max_x = coords[2];
		pdata->max_y = coords[3];
	} else {
		dev_err(dev, "unsupported property %s\n", name);
		return -EINVAL;
	}

	return 0;
}

static int mms_parse_dt(struct device *dev,
			struct mms_ts_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;

	rc = mms_get_dt_coords(dev, "melfas,panel-coords", pdata);
	if (rc)
		return rc;

	/* regulator info */
	pdata->i2c_pull_up = of_property_read_bool(np, "melfas,i2c-pull-up");
	//pdata->vdd_en = of_get_named_gpio(np, "vdd_en-gpio", 0);
	pdata->vdd_en_reg = regulator_get(dev, "vdd_ldo19");
	if (pdata->vdd_en_reg == NULL) {
		pr_err("ERROR: Cannot find vdd_ldo19\n");
	}

	/* reset, irq gpio info */
	pdata->gpio_scl = of_get_named_gpio_flags(np, "melfas,scl-gpio",
				0, &pdata->scl_gpio_flags);
	pdata->gpio_sda = of_get_named_gpio_flags(np, "melfas,sda-gpio",
				0, &pdata->sda_gpio_flags);
	pdata->gpio_int = of_get_named_gpio_flags(np, "melfas,irq-gpio",
				0, &pdata->irq_gpio_flags);
	pdata->config_fw_version = of_get_property(np,
				"melfas,config_fw_version", NULL);
	return 0;
}
#else
static int mms_parse_dt(struct device *dev,
			struct mms_ts_platform_data *pdata)
{
	return -ENODEV;
}
#endif

#ifdef W1_DUMMY
static struct device w1_dev = {
	.init_name = "w1_bus_master1",
};

static ssize_t w1_show(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	return snprintf(buf, 5, "1\n");
}

static DEVICE_ATTR(w1_master_check_id, S_IRUGO, w1_show, NULL);
#endif

//static int __devinit mms_ts_probe(struct i2c_client *client,
int __devinit mms_ts_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct mms_ts_platform_data *pdata;
	struct mms_ts_info *info;
	struct input_dev *input_dev;
	int ret = 0;
//	int val1 = 0, val2 = 0;
//	int fw_core = 0, fw_boot = 0, fw_config = 0;
//	char buf[4] = { 0, };
	int error;

#ifdef SEC_TSP_FACTORY_TEST
	int i;
#if 0 /* ignore sysfs device creation, for now */
	struct device *fac_dev_ts;
#endif /* ignore sysfs device creation, for now */
#endif
	touch_is_pressed = 0;
	printk(KERN_ERR "%s: [TSP] probe START!!\n", __func__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -EIO;

#ifdef CONFIG_OF
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct mms_ts_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		error = mms_parse_dt(&client->dev, pdata);
		if (error)
			return error;
	} else
		pdata = client->dev.platform_data;

	if (!pdata)
		return -EINVAL;

	melfas_request_gpio(pdata);
#endif
	info = kzalloc(sizeof(struct mms_ts_info), GFP_KERNEL);
	if (!info) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&client->dev, "Failed to allocate memory for input device\n");
		ret = -ENOMEM;
		goto err_input_alloc;
	}

	info->client = client;
	info->input_dev = input_dev;
//	info->pdata = client->dev.platform_data;
	info->pdata = pdata;

	if (NULL == info->pdata) {
		pr_err("failed to get platform data\n");
		goto err_config;
	}


	info->irq = -1;
	//mutex_init(&info->lock);
	if (info->pdata) {
		info->max_x = info->pdata->max_x;
		info->max_y = info->pdata->max_y;
		info->invert_x = info->pdata->invert_x;
		info->invert_y = info->pdata->invert_y;
		info->config_fw_version = info->pdata->config_fw_version;
		info->register_cb = melfas_register_callback;
	} else {
		info->max_x = 720;
		info->max_y = 1280;
	}

	melfas_vdd_on(info, 1);
	msleep(100);
	i2c_set_clientdata(client, info);

	info->callbacks.inform_charger = melfas_ta_cb;
	if (info->register_cb)
		info->register_cb(&info->callbacks);

	input_mt_init_slots(input_dev, MAX_FINGERS, 0);
/*
	snprintf(info->phys, sizeof(info->phys),
		 "%s/input0", dev_name(&client->dev));
*/
	input_dev->name = "sec_touchscreen"; /*= "Melfas MMSxxx Touchscreen";*/
	input_dev->phys = info->phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
	input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, MAX_WIDTH, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
				0, info->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
				0, info->max_y, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_TOUCH_MAJOR,
				0, MAX_PRESSURE, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_TOUCH_MINOR,
				0, MAX_PRESSURE, 0, 0);
//	input_set_abs_params(input_dev, ABS_MT_ANGLE,
//				MIN_ANGLE, MAX_ANGLE, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PALM,
				0, 1, 0, 0);
	input_set_drvdata(input_dev, info);

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&client->dev, "failed to register input dev (%d)\n",
			ret);
		goto err_reg_input_dev;
	}

#ifdef TOUCH_BOOSTER
#if TOUCH_BOOSTER
	mutex_init(&info->dvfs_lock);
	INIT_DELAYED_WORK(&info->work_dvfs_off, set_dvfs_off);
	INIT_DELAYED_WORK(&info->work_dvfs_chg, change_dvfs_lock);
	info->dvfs_lock_status = false;
#endif
#endif

	client->irq = gpio_to_irq(pdata->gpio_int);
#if ISC_DL_MODE
	ret = mms_ts_fw_load(info);
#else
	ret = mms_ts_fw_info(info);
#endif
	if (ret)
		dev_err(&client->dev, "failed to check the firmware");

	info->enabled = true;

	printk(KERN_ERR "%s: tsp : gpio_to_irq : %d\n", __func__, client->irq);

	ret = request_threaded_irq(client->irq, NULL, mms_ts_interrupt,
				   IRQF_TRIGGER_LOW  | IRQF_ONESHOT,
				   MELFAS_TS_NAME, info);
	if (ret < 0) {
		dev_err(&client->dev, "%s:Failed to register interrupt, ret = %d\n",
				__func__, ret);
		goto err_req_irq;
	}
	info->irq = client->irq;

#ifdef CONFIG_HAS_EARLYSUSPEND
	info->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	info->early_suspend.suspend = mms_ts_early_suspend;
	info->early_suspend.resume = mms_ts_late_resume;
	register_early_suspend(&info->early_suspend);
#endif

#ifdef USE_OPEN_CLOSE
	input_dev->open = melfas_ts_open;
	input_dev->close = melfas_ts_close;
#endif

#ifdef SEC_TSP_FACTORY_TEST
		INIT_LIST_HEAD(&info->cmd_list_head);
		for (i = 0; i < ARRAY_SIZE(tsp_cmds); i++)
			list_add_tail(&tsp_cmds[i].list, &info->cmd_list_head);

		mutex_init(&info->cmd_lock);
		info->cmd_is_running = false;
#if 0 /* ignore sysfs device creation, for now */
	fac_dev_ts = device_create(sec_class,
			NULL, 0, info, "tsp");
	if (IS_ERR(fac_dev_ts))
		dev_err(&client->dev, "Failed to create device for the sysfs\n");

	ret = sysfs_create_group(&fac_dev_ts->kobj,
			       &sec_touch_factory_attr_group);
	if (ret)
		dev_err(&client->dev, "Failed to create sysfs group\n");
#endif /* ignore sysfs device creation, for now */
#endif

#ifdef W1_DUMMY
	ret = device_register(&w1_dev);
	if(ret){
		pr_err("[W1] error register dummy w1 device\n");
		goto w1_out;
	}
	ret = sysfs_create_file(&w1_dev.kobj, &dev_attr_w1_master_check_id.attr);
	if(ret)
		pr_err("[W1] couldn't create sysfs\n");

	pr_info("[W1] created dummy w1 sysfs\n");
w1_out:
#endif

	printk(KERN_ERR "%s: [TSP] probe END!!\n", __func__);

	return 0;

err_req_irq:
	input_unregister_device(input_dev);
err_reg_input_dev:
err_config:
	input_free_device(input_dev);
err_input_alloc:
	kfree(info->fw_name);
	kfree(info);
err_alloc:
	return ret;
}

static int __devexit mms_ts_remove(struct i2c_client *client)
{
	struct mms_ts_info *info = i2c_get_clientdata(client);

	if (info->irq >= 0)
		free_irq(info->irq, info);
	input_unregister_device(info->input_dev);
	kfree(info->fw_name);
	kfree(info);

	return 0;
}

#if defined(CONFIG_PM) || defined(CONFIG_HAS_EARLYSUSPEND) || defined(USE_OPEN_CLOSE)
static int mms_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mms_ts_info *info = i2c_get_clientdata(client);
//#ifdef SEC_TSP_DEBUG
//	int mt_val;
//#endif

	dev_notice(&info->client->dev, "%s: users=%d\n", __func__,
			info->input_dev->users);
//	mutex_lock(&info->input_dev->mutex);
//	if (!info->input_dev->users)
//		goto out;

	mms_ts_disable(info, 0);
	touch_is_pressed = 0;
	release_all_fingers(info);
	melfas_vdd_on(info, 0);
	msleep(50);

//out:
//	mutex_unlock(&info->input_dev->mutex);
	return 0;
}

static int mms_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mms_ts_info *info = i2c_get_clientdata(client);
	int ret = 0;

	dev_notice(&info->client->dev, "%s: users=%d\n", __func__,
			info->input_dev->users);
	melfas_vdd_on(info, 1);
	msleep(50);

	if (info->ta_status) {
		dev_notice(&client->dev, "TA connect!!!\n");
		i2c_smbus_write_byte_data(info->client, 0x33, 0x1);
	} else {
		dev_notice(&client->dev, "TA disconnect!!!\n");
		i2c_smbus_write_byte_data(info->client, 0x33, 0x2);
	}

	mms_set_noise_mode(info);
//	mutex_lock(&info->input_dev->mutex);
//	if (info->input_dev->users)
#ifdef SEC_TSP_FACTORY_TEST
	cover_set(info, info->cover_state);
#endif
		ret = mms_ts_enable(info, 0);
//	mutex_unlock(&info->input_dev->mutex);

	return ret;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mms_ts_early_suspend(struct early_suspend *h)
{
	struct mms_ts_info *info;
	info = container_of(h, struct mms_ts_info, early_suspend);
	mms_ts_suspend(&info->client->dev);
}

static void mms_ts_late_resume(struct early_suspend *h)
{
	struct mms_ts_info *info;
	info = container_of(h, struct mms_ts_info, early_suspend);
	mms_ts_resume(&info->client->dev);
}
#endif

#ifdef USE_OPEN_CLOSE
static void  melfas_ts_close(struct input_dev *dev)
{
	struct mms_ts_info *info = input_get_drvdata(dev);
	printk(KERN_ERR "[TSP], %s, %d\n",__func__, __LINE__);
	mms_ts_suspend(&info->client->dev);
}
static int  melfas_ts_open(struct input_dev *dev)
{
	struct mms_ts_info *info = input_get_drvdata(dev);
	printk(KERN_ERR "[TSP], %s, %d\n",__func__, __LINE__);
	mms_ts_resume(&info->client->dev);
	return 0;
}
#endif

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND) && !defined(USE_OPEN_CLOSE)
static const struct dev_pm_ops mms_ts_pm_ops = {
	.suspend	= mms_ts_suspend,
	.resume		= mms_ts_resume,
};
#endif

static const struct i2c_device_id mms_ts_id[] = {
	{MELFAS_TS_NAME, 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, mms_ts_id);

#ifdef CONFIG_OF
static struct of_device_id mms_match_table[] = {
	{ .compatible = "melfas,mms-ts",},
	{ },
};
#else
#define mms_match_table NULL
#endif

static struct i2c_driver mms_ts_driver = {
	.probe		= mms_ts_probe,
	.remove		= __devexit_p(mms_ts_remove),
	.driver = {
		   .name = MELFAS_TS_NAME,
#ifdef CONFIG_OF
		   .of_match_table = mms_match_table,
#endif
#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND) && !defined(USE_OPEN_CLOSE)
		.pm	= &mms_ts_pm_ops,
#endif
	},
	.id_table	= mms_ts_id,
};

static int __init mms_ts_init(void)
{
#if 0 /* reference to battery driver variable */
	if (poweroff_charging) {
		pr_info("%s : LPM Charging Mode!!\n", __func__);
		return 0;
	}
#endif /* reference to battery driver variable */
	return i2c_add_driver(&mms_ts_driver);
}

static void __exit mms_ts_exit(void)
{
	i2c_del_driver(&mms_ts_driver);
}

module_init(mms_ts_init);
module_exit(mms_ts_exit);

/* Module information */
MODULE_DESCRIPTION("Touchscreen driver for Melfas MMS-series controllers");
MODULE_LICENSE("GPL");
