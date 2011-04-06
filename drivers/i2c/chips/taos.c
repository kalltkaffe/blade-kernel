/******************************************************************
*File Name: 	taos.c 	                                           *
*Description:	Linux device driver for Taos ambient light and    *
*			proximity sensors.                                          *                                
******************************************************************/
// includes
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/timer.h>
#include <asm/uaccess.h>
#include <asm/errno.h>
#include <asm/delay.h>
#include <linux/irq.h>
#include <asm/gpio.h>
#include "taos_common.h"
#include <linux/input.h>
#include <linux/miscdevice.h>
#ifdef CONFIG_PROX_WAKE_LOCK
#include <linux/wakelock.h>
#endif

#define TAOS_INT_GPIO 42
#define TAOS_TAG        "[taos]"


#define TAOS_DEVICE_NAME		"taos"
#define TAOS_DEVICE_ID			"taos"
#define TAOS_ID_NAME_SIZE		10
#define TAOS_TRITON_CHIPIDVAL		0x00
#define TAOS_TRITON_MAXREGS		32
#define TAOS_DEVICE_ADDR1		0x29
#define TAOS_DEVICE_ADDR2		0x39
#define TAOS_DEVICE_ADDR3		0x49
#define TAOS_MAX_NUM_DEVICES		3
#define TAOS_MAX_DEVICE_REGS		32
#define I2C_MAX_ADAPTERS		8

// TRITON register offsets
#define TAOS_TRITON_CNTRL		0x00
#define TAOS_TRITON_ALS_TIME		0X01
#define TAOS_TRITON_PRX_TIME		0x02
#define TAOS_TRITON_WAIT_TIME		0x03
#define TAOS_TRITON_ALS_MINTHRESHLO	0X04
#define TAOS_TRITON_ALS_MINTHRESHHI 	0X05
#define TAOS_TRITON_ALS_MAXTHRESHLO	0X06
#define TAOS_TRITON_ALS_MAXTHRESHHI	0X07
#define TAOS_TRITON_PRX_MINTHRESHLO 	0X08
#define TAOS_TRITON_PRX_MINTHRESHHI 	0X09
#define TAOS_TRITON_PRX_MAXTHRESHLO 	0X0A
#define TAOS_TRITON_PRX_MAXTHRESHHI 	0X0B
#define TAOS_TRITON_INTERRUPT		0x0C
#define TAOS_TRITON_PRX_CFG		0x0D
#define TAOS_TRITON_PRX_COUNT		0x0E
#define TAOS_TRITON_GAIN		0x0F
#define TAOS_TRITON_REVID		0x11
#define TAOS_TRITON_CHIPID      	0x12
#define TAOS_TRITON_STATUS		0x13
#define TAOS_TRITON_ALS_CHAN0LO		0x14
#define TAOS_TRITON_ALS_CHAN0HI		0x15
#define TAOS_TRITON_ALS_CHAN1LO		0x16
#define TAOS_TRITON_ALS_CHAN1HI		0x17
#define TAOS_TRITON_PRX_LO		0x18
#define TAOS_TRITON_PRX_HI		0x19
#define TAOS_TRITON_TEST_STATUS		0x1F

// Triton cmd reg masks
#define TAOS_TRITON_CMD_REG		0X80
#define TAOS_TRITON_CMD_BYTE_RW		0x00
#define TAOS_TRITON_CMD_WORD_BLK_RW	0x20
#define TAOS_TRITON_CMD_SPL_FN		0x60
#define TAOS_TRITON_CMD_PROX_INTCLR	0X05
#define TAOS_TRITON_CMD_ALS_INTCLR	0X06
#define TAOS_TRITON_CMD_PROXALS_INTCLR 	0X07
#define TAOS_TRITON_CMD_TST_REG		0X08
#define TAOS_TRITON_CMD_USER_REG	0X09

// Triton cntrl reg masks
#define TAOS_TRITON_CNTL_PROX_INT_ENBL	0X20
#define TAOS_TRITON_CNTL_ALS_INT_ENBL	0X10
#define TAOS_TRITON_CNTL_WAIT_TMR_ENBL	0X08
#define TAOS_TRITON_CNTL_PROX_DET_ENBL	0X04
#define TAOS_TRITON_CNTL_ADC_ENBL	0x02
#define TAOS_TRITON_CNTL_PWRON		0x01

// Triton status reg masks
#define TAOS_TRITON_STATUS_ADCVALID	0x01
#define TAOS_TRITON_STATUS_PRXVALID	0x02
#define TAOS_TRITON_STATUS_ADCINTR	0x10
#define TAOS_TRITON_STATUS_PRXINTR	0x20

// lux constants
#define	TAOS_MAX_LUX			65535000
#define TAOS_SCALE_MILLILUX		3
#define TAOS_FILTER_DEPTH		3

// forward declarations
static int taos_probe(struct i2c_client *clientp, const struct i2c_device_id *idp);
static int taos_remove(struct i2c_client *client);
static int taos_open(struct inode *inode, struct file *file);
static int taos_release(struct inode *inode, struct file *file);
static int taos_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);
static int taos_read(struct file *file, char *buf, size_t count, loff_t *ppos);
static int taos_write(struct file *file, const char *buf, size_t count, loff_t *ppos);
static loff_t taos_llseek(struct file *file, loff_t offset, int orig);
static int taos_get_lux(void);
static int taos_lux_filter(int raw_lux);
static int taos_device_name(unsigned char *bufp, char **device_name);
static int taos_prox_poll(struct taos_prox_info *prxp);
static void taos_als_work(struct work_struct *w);
static void taos_prox_work(struct work_struct *w);
static void taos_report_value(int mask);
static int calc_distance(int value);
#ifdef CONFIG_PROX_WAKE_LOCK
static struct wake_lock taos_wake_lock;
#endif
static int light_on= 0;
static int prox_on = 0;

struct alsprox_data {
	struct input_dev *input_dev;
};

static struct alsprox_data *alsprox;

// first device number
static dev_t taos_dev_number;

// class structure for this device
struct class *taos_class;

// module device table
static struct i2c_device_id taos_idtable[] = {
        {TAOS_DEVICE_ID, 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, taos_idtable);

// board and address info
struct i2c_board_info taos_board_info[] = {
	{I2C_BOARD_INFO(TAOS_DEVICE_ID, TAOS_DEVICE_ADDR1),},
	{I2C_BOARD_INFO(TAOS_DEVICE_ID, TAOS_DEVICE_ADDR2),},
	{I2C_BOARD_INFO(TAOS_DEVICE_ID, TAOS_DEVICE_ADDR3),},
};
unsigned short const taos_addr_list[4] = {TAOS_DEVICE_ADDR1, TAOS_DEVICE_ADDR2, TAOS_DEVICE_ADDR3, I2C_CLIENT_END};

// client and device
struct i2c_client *my_clientp;
struct i2c_client *bad_clientp[TAOS_MAX_NUM_DEVICES];

// driver definition
static struct i2c_driver taos_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "taos",
	 },
	.id_table = taos_idtable,
	.probe = taos_probe,
	.remove = __devexit_p(taos_remove),
};


struct taos_intr_data {
    int int_gpio;
    int irq;
};

// per-device data
struct taos_data {
	struct i2c_client *client;
	struct cdev cdev;
	unsigned int addr;
	char taos_id;
	char taos_name[TAOS_ID_NAME_SIZE];
	struct semaphore update_lock;
	char valid;
	unsigned long last_updated;
    	struct taos_intr_data *pdata;
	struct delayed_work  als_work;	
	struct work_struct  prox_work;
} *taos_datap;

static struct taos_intr_data taos_irq= {
    .int_gpio = TAOS_INT_GPIO,
    .irq = MSM_GPIO_TO_INT(TAOS_INT_GPIO),
};


// file operations
static struct file_operations taos_fops = {
	.owner = THIS_MODULE,
	.open = taos_open,
	.release = taos_release,
	.read = taos_read,
	.write = taos_write,
	.llseek = taos_llseek,
	.ioctl = taos_ioctl,
};

// device configuration
struct taos_cfg *taos_cfgp;
static u32 calibrate_target_param = 300000;
static u16 als_time_param = 100;
static u16 scale_factor_param = 1;
static u16 gain_trim_param = 512;
static u8 filter_history_param = 3;
static u8 filter_count_param = 1;
static u8 gain_param = 1;
#if defined(CONFIG_MACH_BLADE)
static u16 prox_threshold_hi_param = 8500; 
static u16 prox_threshold_lo_param = 8000;
#elif defined(CONFIG_MACH_JOE)
static u16 prox_threshold_hi_param = 4500; 
static u16 prox_threshold_lo_param = 4000;
#else
static u16 prox_threshold_hi_param = 9500; 
static u16 prox_threshold_lo_param = 9000;
#endif
static u8 prox_int_time_param = 0xEE;
static u8 prox_adc_time_param = 0xEE;
static u8 prox_wait_time_param = 0xF6;
static u8 prox_intr_filter_param = 0x00;
static u8 prox_config_param = 0x00;
static u8 prox_pulse_cnt_param = 0x08;
static u8 prox_gain_param = 0x20;

// device reg init values
u8 taos_triton_reg_init[16] = {0x00,0xFF,0XFF,0XFF,0X00,0X00,0XFF,0XFF,0X00,0X00,0XFF,0XFF,0X00,0X00,0X00,0X00};

static u16 als_intr_threshold_hi_param = 0;
static u16 als_intr_threshold_lo_param = 0;
int g_nlux = 0;


// prox info
struct taos_prox_info prox_cal_info[20];
struct taos_prox_info prox_cur_info;
struct taos_prox_info *prox_cur_infop = &prox_cur_info;

static int device_released = 0;
static u16 sat_als = 0;
static u16 sat_prox = 0;

// lux time scale
struct time_scale_factor  {
	u16	numerator;
	u16	denominator;
	u16	saturation;
};
struct time_scale_factor TritonTime = {1, 0, 0};
struct time_scale_factor *lux_timep = &TritonTime;

// gain table
u8 taos_triton_gain_table[] = {1, 8, 16, 120};

// lux data
struct lux_data {
	u16	ratio;
	u16	clear;
	u16	ir;
};
struct lux_data TritonFN_lux_data[] = {
        { 9830,  8320,  15360 },
        { 12452, 10554, 22797 },
        { 14746, 6234,  11430 },
        { 17695, 3968,  6400  },
        { 0,     0,     0     }
};
struct lux_data *lux_tablep = TritonFN_lux_data;
static int lux_history[TAOS_FILTER_DEPTH] = {-ENODATA, -ENODATA, -ENODATA};


int taos_config_int_gpio(int int_gpio)
{
    int rc=0;
    uint32_t  gpio_config_data = GPIO_CFG(int_gpio,  0, GPIO_INPUT, GPIO_PULL_UP, GPIO_2MA);

    rc = gpio_request(int_gpio, "gpio_sensor");
    if (rc) {
        printk(TAOS_TAG "%s: gpio_request(%#x)=%d\n",
                __func__, int_gpio, rc);
        return rc;
    }

    rc = gpio_tlmm_config(gpio_config_data, GPIO_ENABLE);
    if (rc) {
        printk(TAOS_TAG "%s: gpio_tlmm_config(%#x)=%d\n",
                __func__, gpio_config_data, rc);
        return rc;
    }

    mdelay(1);

    rc = gpio_direction_input(int_gpio);
    if (rc) {
        printk(TAOS_TAG "%s: gpio_direction_input(%#x)=%d\n",
                __func__, int_gpio, rc);
        return rc;
    }

    return 0;
}

static irqreturn_t taos_interrupt(int irq, void *data)
{
    u16 status = 0;
    int ret =0;	
    disable_irq_nosync(taos_datap->pdata->irq);
    if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | 0x13)))) < 0) {                    
	goto read_reg_fail;
	}	
    status = i2c_smbus_read_byte(taos_datap->client);

    if(((status & 0x10) != 0)&&((status & 0x20)==0))
	schedule_delayed_work(&taos_datap->als_work,0);

    if((status & 0x20)!=0) 
	schedule_work(&taos_datap->prox_work);

read_reg_fail:
	enable_irq(taos_datap->pdata->irq);
    return IRQ_HANDLED;
}



static void taos_als_work(struct work_struct *w)
{

	int ret =0;	
    	g_nlux = taos_get_lux();
    	if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG|TAOS_TRITON_CMD_SPL_FN|TAOS_TRITON_CMD_PROXALS_INTCLR)))) < 0) {
   		printk(KERN_CRIT "TAOS: i2c_smbus_write_byte failed in clear als interrupt\n");
	}
		
	taos_report_value(0);
}

static void taos_prox_work(struct work_struct *w)
{
	int ret =0;
	int prx_hi, prx_lo;
	if((ret = taos_prox_poll(prox_cur_infop))<0)
		printk(KERN_CRIT "TAOS: get prox poll  failed in  taos interrupt()\n");  	
	if(prox_cur_infop->prox_data > taos_cfgp->prox_threshold_hi)       
	 {           
	 	if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MAXTHRESHLO),sat_prox))) < 0) {        	        
			pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MAXTHRESHLO\n");                	
			    	
			}           
		if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MINTHRESHLO),taos_cfgp->prox_threshold_lo))) < 0) {        	       
			pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MINTHRESHLO\n");                	
			       
			}            
		prox_cur_infop->prox_event = 1;                     
	}        
	else if(prox_cur_infop->prox_data < taos_cfgp->prox_threshold_lo)        
	{            
		if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MAXTHRESHLO),taos_cfgp->prox_threshold_hi))) < 0) {    	       
			pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MAXTHRESHLO\n");            	
			      	 
			}             
		if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MINTHRESHLO),0))) < 0) {    	    
			pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MINTHRESHLO\n");            
			    	 
			}             
		prox_cur_infop->prox_event = 0;                             
	}   
      
	 if ((prx_hi= (i2c_smbus_read_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MAXTHRESHLO)))) < 0) {        	        
		pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MAXTHRESHLO\n");                	
			    	
		}           
	if ((prx_lo = (i2c_smbus_read_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MINTHRESHLO)))) < 0) {        	       
		pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MINTHRESHLO\n");                	
			       
		}    

	if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG|TAOS_TRITON_CMD_SPL_FN|TAOS_TRITON_CMD_PROXALS_INTCLR)))) < 0) {        
		pr_crit(TAOS_TAG "i2c_smbus_write_byte failed in clear interrupt\n");                                   
	}    
	
	taos_report_value(1);
}


static int calc_distance(int value)
{
	int temp=0;
	if(prox_cur_infop->prox_event == 1)
		temp=4-((value-prox_threshold_lo_param)/3000);
	else if(prox_cur_infop->prox_event == 0)
		temp=5;
        return temp;
}

static void taos_report_value(int mask)
{
	struct taos_prox_info *val = prox_cur_infop;
	int lux_val=g_nlux;
	int  dist;
	lux_val/=25;
	dist=calc_distance(val->prox_data);

	if (mask==0) {
		input_report_abs(alsprox->input_dev, ABS_X, lux_val);
	}

	if (mask==1) {
		input_report_abs(alsprox->input_dev, ABS_Y, dist);
		printk(KERN_CRIT "TAOS: prox_interrupt =%d, distance=%d\n",  val->prox_data,dist);
	}

	input_sync(alsprox->input_dev);	
}


static int __init taos_init(void) {
	int ret = 0;
	printk(KERN_ERR "TAOS: comes into taos_init\n");
	if ((ret = (alloc_chrdev_region(&taos_dev_number, 0, TAOS_MAX_NUM_DEVICES, TAOS_DEVICE_NAME))) < 0) {
		printk(KERN_ERR "TAOS: alloc_chrdev_region() failed in taos_init()\n");
                return (ret);
	}
        taos_class = class_create(THIS_MODULE, TAOS_DEVICE_NAME);
        taos_datap = kmalloc(sizeof(struct taos_data), GFP_KERNEL);
        if (!taos_datap) {
		printk(KERN_ERR "TAOS: kmalloc for struct taos_data failed in taos_init()\n");
                return -ENOMEM;
	}
        memset(taos_datap, 0, sizeof(struct taos_data));
        cdev_init(&taos_datap->cdev, &taos_fops);
        taos_datap->cdev.owner = THIS_MODULE;
        if ((ret = (cdev_add(&taos_datap->cdev, taos_dev_number, 1))) < 0) {
		printk(KERN_ERR "TAOS: cdev_add() failed in taos_init()\n");
                return (ret);
	}
	device_create(taos_class, NULL, MKDEV(MAJOR(taos_dev_number), 0), &taos_driver ,"taos");
        ret = i2c_add_driver(&taos_driver);
	if(ret){
		printk(KERN_ERR "TAOS: i2c_add_driver() failed in taos_init(),%d\n",ret);
                return (ret);
	}
        return (ret);
}


static void __exit taos_exit(void) {
        i2c_del_driver(&taos_driver);
        unregister_chrdev_region(taos_dev_number, TAOS_MAX_NUM_DEVICES);
	device_destroy(taos_class, MKDEV(MAJOR(taos_dev_number), 0));
	cdev_del(&taos_datap->cdev);
	class_destroy(taos_class);
        kfree(taos_datap);
}


// client probe
static int taos_probe(struct i2c_client *clientp, const struct i2c_device_id *idp) {
	int ret = 0;
	int i = 0;
	unsigned char buf[TAOS_MAX_DEVICE_REGS];
	char *device_name;

	if (!i2c_check_functionality(clientp->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		printk(KERN_ERR "TAOS: taos_probe() - i2c smbus byte data functions unsupported\n");
		return -EOPNOTSUPP;
		}
    if (!i2c_check_functionality(clientp->adapter, I2C_FUNC_SMBUS_WORD_DATA)) {
		printk(KERN_ERR "TAOS: taos_probe() - i2c smbus word data functions unsupported\n");
        }
    if (!i2c_check_functionality(clientp->adapter, I2C_FUNC_SMBUS_BLOCK_DATA)) {
		printk(KERN_ERR "TAOS: taos_probe() - i2c smbus block data functions unsupported\n");
        }
	taos_datap->client = clientp;
	i2c_set_clientdata(clientp, taos_datap);
	
	//write bytes to address control registers
    for(i = 0; i < TAOS_MAX_DEVICE_REGS; i++) {
		if((ret = (i2c_smbus_write_byte(clientp, (TAOS_TRITON_CMD_REG | (TAOS_TRITON_CNTRL + i))))) < 0) {
			printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to address control regs failed in taos_probe()\n");
			return(ret);
        }
		buf[i] = i2c_smbus_read_byte(clientp);
	}	
	//check device ID "tritonFN"
	if ((ret = taos_device_name(buf, &device_name)) == 0) {
		printk(KERN_ERR "TAOS: chip id that was read found mismatched by taos_device_name(), in taos_probe()\n");
 		return -ENODEV;
        }
	if (strcmp(device_name, TAOS_DEVICE_ID)) {
		printk(KERN_ERR "TAOS: chip id that was read does not match expected id in taos_probe()\n");
		return -ENODEV;
        }
	else{
		printk(KERN_ERR "TAOS: chip id of %s that was read matches expected id in taos_probe()\n", device_name);
	}

	if ((ret = (i2c_smbus_write_byte(clientp, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL)))) < 0) {
		printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to control reg failed in taos_probe()\n");
		return(ret);
        }
	if ((ret = (i2c_smbus_write_word_data(clientp, (0xA0 | TAOS_TRITON_ALS_MAXTHRESHLO),0))) < 0) {
        	printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to chan0/1/lo/hi reg failed in taos_probe()\n");
        }
    	if ((ret = (i2c_smbus_write_word_data(clientp, (0xA0 | TAOS_TRITON_ALS_MINTHRESHLO),0))) < 0) {
        	printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to chan0/1/lo/hi reg failed in taos_probe()\n");
        }
	
	INIT_DELAYED_WORK(&taos_datap->als_work, taos_als_work);
	INIT_WORK(&taos_datap->prox_work, taos_prox_work); 

	taos_datap->pdata = &taos_irq;
    	ret=taos_config_int_gpio(taos_datap->pdata->int_gpio);
    	if (ret) {
		printk(KERN_CRIT "taos configure int_gpio%d failed\n",
                taos_datap->pdata->int_gpio);
        	return ret;
    	}

    	ret = request_irq(taos_datap->pdata->irq, taos_interrupt, IRQF_TRIGGER_FALLING, 
			taos_datap->taos_name, prox_cur_infop);
    	if (ret) {
		printk(KERN_CRIT "taos request interrupt failed\n");
        	return ret;
    	}
	
	strlcpy(clientp->name, TAOS_DEVICE_ID, I2C_NAME_SIZE);
	strlcpy(taos_datap->taos_name, TAOS_DEVICE_ID, TAOS_ID_NAME_SIZE);
	taos_datap->valid = 0;
	
	init_MUTEX(&taos_datap->update_lock);
	if (!(taos_cfgp = kmalloc(sizeof(struct taos_cfg), GFP_KERNEL))) {
		printk(KERN_ERR "TAOS: kmalloc for struct taos_cfg failed in taos_probe()\n");
		return -ENOMEM;
	}
	taos_cfgp->calibrate_target = calibrate_target_param;
	taos_cfgp->als_time = als_time_param;
	taos_cfgp->scale_factor = scale_factor_param;
	taos_cfgp->gain_trim = gain_trim_param;
	taos_cfgp->filter_history = filter_history_param;
	taos_cfgp->filter_count = filter_count_param;
	taos_cfgp->gain = gain_param;
	taos_cfgp->prox_threshold_hi = prox_threshold_hi_param;
	taos_cfgp->prox_threshold_lo = prox_threshold_lo_param;
	taos_cfgp->prox_int_time = prox_int_time_param;
	taos_cfgp->prox_adc_time = prox_adc_time_param;
	taos_cfgp->prox_wait_time = prox_wait_time_param;
	taos_cfgp->prox_intr_filter = prox_intr_filter_param;
	taos_cfgp->prox_config = prox_config_param;
	taos_cfgp->prox_pulse_cnt = prox_pulse_cnt_param;
	taos_cfgp->prox_gain = prox_gain_param;
	sat_als = (256 - taos_cfgp->prox_int_time) << 10;
	sat_prox = (256 - taos_cfgp->prox_adc_time) << 10;

	alsprox = kzalloc(sizeof(struct alsprox_data), GFP_KERNEL);
	if (!alsprox) {
		ret = -ENOMEM;
		goto exit_alloc_data_failed;
	}

	alsprox->input_dev = input_allocate_device();
	if (!alsprox->input_dev) {
		ret = -ENOMEM;
		printk(KERN_ERR "taos_probe: Failed to allocate input device\n");
		goto exit_input_dev_alloc_failed;
	}

	set_bit(EV_ABS, alsprox->input_dev->evbit);	
	input_set_abs_params(alsprox->input_dev,  ABS_X, 0, 65535, 0, 0);
	input_set_abs_params(alsprox->input_dev, ABS_Y, 0, 65535, 0, 0);
	alsprox->input_dev->name = "alsprox";
	ret = input_register_device(alsprox->input_dev);
	if (ret) {
		printk("taos_probe: Unable to register input device: %s\n", alsprox->input_dev->name);
		goto exit_input_register_device_failed;
	}	
	return (ret);

exit_input_register_device_failed:
	input_free_device(alsprox->input_dev);
	
exit_input_dev_alloc_failed:
exit_alloc_data_failed:
	kfree(alsprox);
	return (ret);		
}


// client remove
static int __devexit taos_remove(struct i2c_client *client) {
	int ret = 0;

	return (ret);
}

// open
static int taos_open(struct inode *inode, struct file *file) {
	struct taos_data *taos_datap;
	int ret = 0;

	device_released = 0;
    printk(KERN_CRIT "[taos]:%s\n", __func__);
	taos_datap = container_of(inode->i_cdev, struct taos_data, cdev);
	if (strcmp(taos_datap->taos_name, TAOS_DEVICE_ID) != 0) {
		printk(KERN_ERR "TAOS: device name incorrect during taos_open(), get %s\n", taos_datap->taos_name);
		ret = -ENODEV;
	}
	return (ret);
}

// release
static int taos_release(struct inode *inode, struct file *file) {
	struct taos_data *taos_datap;
	int ret = 0;

	device_released = 1;
	prox_on = 0;
	    printk(KERN_CRIT "[taos]:%s\n", __func__);

	taos_datap = container_of(inode->i_cdev, struct taos_data, cdev);
	if (strcmp(taos_datap->taos_name, TAOS_DEVICE_ID) != 0) {
		printk(KERN_ERR "TAOS: device name incorrect during taos_release(), get %s\n", taos_datap->taos_name);
		ret = -ENODEV;
	}
	return (ret);
}

// read
static int taos_read(struct file *file, char *buf, size_t count, loff_t *ppos) {
	struct taos_data *taos_datap;
	u8 i = 0, xfrd = 0, reg = 0;
	u8 my_buf[TAOS_MAX_DEVICE_REGS];
	int ret = 0;

        if ((*ppos < 0) || (*ppos >= TAOS_MAX_DEVICE_REGS)  || (count > TAOS_MAX_DEVICE_REGS)) {
		printk(KERN_ERR "TAOS: reg limit check failed in taos_read()\n");
		return -EINVAL;
	}
	reg = (u8)*ppos;
	taos_datap = container_of(file->f_dentry->d_inode->i_cdev, struct taos_data, cdev);
	while (xfrd < count) {
			if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | reg)))) < 0) {
			printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in taos_read()\n");
			return (ret);
			}
      		my_buf[i++] = i2c_smbus_read_byte(taos_datap->client);
			reg++;
			xfrd++;
        }
        if ((ret = copy_to_user(buf, my_buf, xfrd))){
		printk(KERN_ERR "TAOS: copy_to_user failed in taos_read()\n");
		return -ENODATA;
	}
        return ((int)xfrd);
}

// write
static int taos_write(struct file *file, const char *buf, size_t count, loff_t *ppos) {
	struct taos_data *taos_datap;
	u8 i = 0, xfrd = 0, reg = 0;
	u8 my_buf[TAOS_MAX_DEVICE_REGS];
	int ret = 0;

        if ((*ppos < 0) || (*ppos >= TAOS_MAX_DEVICE_REGS) || ((*ppos + count) > TAOS_MAX_DEVICE_REGS)) {
		printk(KERN_ERR "TAOS: reg limit check failed in taos_write()\n");
		return -EINVAL;
	}
	reg = (u8)*ppos;
        if ((ret =  copy_from_user(my_buf, buf, count))) {
		printk(KERN_ERR "TAOS: copy_to_user failed in taos_write()\n");
 		return -ENODATA;
	}
        taos_datap = container_of(file->f_dentry->d_inode->i_cdev, struct taos_data, cdev);
        while (xfrd < count) {
                if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG | reg), my_buf[i++]))) < 0) {
                        printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in taos_write()\n");
                        return (ret);
                }
                reg++;
                xfrd++;
        }

        return ((int)xfrd);
}

// llseek
static loff_t taos_llseek(struct file *file, loff_t offset, int orig) {
	int ret = 0;
	loff_t new_pos = 0;

	if ((offset >= TAOS_MAX_DEVICE_REGS) || (orig < 0) || (orig > 1)) {
                printk(KERN_ERR "TAOS: offset param limit or origin limit check failed in taos_llseek()\n");
                return -EINVAL;
        }
        switch (orig) {
        	case 0:
                	new_pos = offset;
                	break;
        	case 1:
                	new_pos = file->f_pos + offset;
	                break;
		default:
			return -EINVAL;
			break;
	}
	if ((new_pos < 0) || (new_pos >= TAOS_MAX_DEVICE_REGS) || (ret < 0)) {
		printk(KERN_ERR "TAOS: new offset limit or origin limit check failed in taos_llseek()\n");
		return -EINVAL;
	}
	file->f_pos = new_pos;
	return new_pos;
}

// ioctls
static int taos_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg) {
	struct taos_data *taos_datap;
	int prox_sum = 0, prox_mean = 0, prox_max = 0;
	int lux_val = 0, ret = 0, i = 0, tmp = 0;
	u16 gain_trim_val = 0;
	u8 itime = 0, reg_val = 0, reg_cntrl = 0;

	taos_datap = container_of(inode->i_cdev, struct taos_data, cdev);
	switch (cmd) {
		case TAOS_IOCTL_ALS_ON:			
			for (i = 0; i < TAOS_FILTER_DEPTH; i++)
				lux_history[i] = -ENODATA;
			if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x00), 0x00))) < 0) {
				printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG|TAOS_TRITON_CMD_SPL_FN|TAOS_TRITON_CMD_ALS_INTCLR)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_on\n");
                                return (ret);
                        }
			itime = (((taos_cfgp->als_time/50) * 18) - 1);
			itime = (~itime);
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_ALS_TIME), itime))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl als_on\n");
                                return (ret);
                        }
			if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x03), 0xDC))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl als_on\n");
                                return (ret);
			}			
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_GAIN)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_on\n");
                                return (ret);
                        }
                        reg_val = i2c_smbus_read_byte(taos_datap->client);
			reg_val = reg_val & 0xFC;
			reg_val = reg_val | (taos_cfgp->gain & 0x03);
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_GAIN), reg_val))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl als gain\n");
                                return (ret);
                        }
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl address reg 0x00\n");
                                return (ret);
                        }
                        reg_val = i2c_smbus_read_byte(taos_datap->client);
			if (reg_val & TAOS_TRITON_CNTL_PWRON )
				reg_cntrl = (TAOS_TRITON_CNTL_ADC_ENBL| TAOS_TRITON_CNTL_ALS_INT_ENBL|TAOS_TRITON_CNTL_WAIT_TMR_ENBL);
			else
				reg_cntrl = (TAOS_TRITON_CNTL_ADC_ENBL | TAOS_TRITON_CNTL_PWRON | TAOS_TRITON_CNTL_ALS_INT_ENBL|TAOS_TRITON_CNTL_WAIT_TMR_ENBL);
			if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL), reg_cntrl))) < 0) {
	                        printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl als enable\n");
        	                return (ret);
                	}
           		if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_INTERRUPT), 0x03))) < 0) {
	                        printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl als_on\n");
        	                return (ret);
                	}
			light_on=1;	
            		pr_crit(TAOS_TAG "TAOS_IOCTL_ALS_ON,lux=%d\n", g_nlux);
			return (ret);
			break;
                case TAOS_IOCTL_ALS_OFF:	
                        for (i = 0; i < TAOS_FILTER_DEPTH; i++)
                                lux_history[i] = -ENODATA;
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_calibrate\n");
                                return (ret);
                        }
                        reg_val = i2c_smbus_read_byte(taos_datap->client);
			if (reg_val & TAOS_TRITON_CNTL_PROX_DET_ENBL)
				reg_cntrl = reg_val & (~TAOS_TRITON_CNTL_ADC_ENBL);
			else
				reg_cntrl = reg_val & (~(TAOS_TRITON_CNTL_ADC_ENBL | TAOS_TRITON_CNTL_PWRON));
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL), reg_cntrl))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl als_off\n");
                                return (ret);
                        }
			light_on=0;	
			pr_crit(TAOS_TAG "TAOS_IOCTL_ALS_OFF\n");		
			return (ret);
                	break;
		case TAOS_IOCTL_ALS_DATA:
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_data\n");
                                return (ret);
                        }
			reg_val = i2c_smbus_read_byte(taos_datap->client);
                        if ((reg_val & (TAOS_TRITON_CNTL_ADC_ENBL | TAOS_TRITON_CNTL_PWRON)) != (TAOS_TRITON_CNTL_ADC_ENBL | TAOS_TRITON_CNTL_PWRON))
                                return -ENODATA;
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_STATUS)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_data\n");
                                return (ret);
                        }
                        reg_val = i2c_smbus_read_byte(taos_datap->client);
			if ((reg_val & TAOS_TRITON_STATUS_ADCVALID) != TAOS_TRITON_STATUS_ADCVALID)
				return -ENODATA;
			if ((lux_val = taos_get_lux()) < 0)
				printk(KERN_ERR "TAOS: call to taos_get_lux() returned error %d in ioctl als_data\n", lux_val);
			lux_val = taos_lux_filter(lux_val);
			return (lux_val);
			break;
		case TAOS_IOCTL_ALS_CALIBRATE:
                        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_CNTRL)))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_calibrate\n");
                                return (ret);
                        }
			reg_val = i2c_smbus_read_byte(taos_datap->client);
			if ((reg_val & 0x07) != 0x07)
				return -ENODATA;
	                if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | TAOS_TRITON_STATUS)))) < 0) {
        	        	printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_calibrate\n");
                                return (ret);
                        }
			reg_val = i2c_smbus_read_byte(taos_datap->client);
			if ((reg_val & 0x01) != 0x01)
                                return -ENODATA;
                        if ((lux_val = taos_get_lux()) < 0) {
                                printk(KERN_ERR "TAOS: call to lux_val() returned error %d in ioctl als_data\n", lux_val);
	                        return (lux_val);
			}
			gain_trim_val = (u16)(((taos_cfgp->calibrate_target) * 512)/lux_val);
			taos_cfgp->gain_trim = (int)gain_trim_val;		
			return ((int)gain_trim_val);
			break;
		case TAOS_IOCTL_CONFIG_GET:
			ret = copy_to_user((struct taos_cfg *)arg, taos_cfgp, sizeof(struct taos_cfg));
			if (ret) {
				printk(KERN_ERR "TAOS: copy_to_user failed in ioctl config_get\n");
				return -ENODATA;
			}
			return (ret);
			break;
                case TAOS_IOCTL_CONFIG_SET:
                        ret = copy_from_user(taos_cfgp, (struct taos_cfg *)arg, sizeof(struct taos_cfg));
			if (ret) {
				printk(KERN_ERR "TAOS: copy_from_user failed in ioctl config_set\n");
                                return -ENODATA;
			}
    		if(taos_cfgp->als_time < 50)
		               	taos_cfgp->als_time = 50;
			if(taos_cfgp->als_time > 650)
		               	taos_cfgp->als_time = 650;
			tmp = (taos_cfgp->als_time + 25)/50;
        		taos_cfgp->als_time = tmp*50;
		        sat_als = (256 - taos_cfgp->prox_int_time) << 10;
	        sat_prox = (256 - taos_cfgp->prox_adc_time) << 10;
                	break;
		case TAOS_IOCTL_PROX_ON:
#ifdef CONFIG_PROX_WAKE_LOCK
			// Use wake lock to stop suspending during calls. This should not be necessary and means that if an app leaves the prox sensor on 
			// the device will not suspend, but that should not happens. Apps should always turn off sensors when not needed.
			pr_crit(TAOS_TAG "get wake lock");
			wake_lock_init(&taos_wake_lock, WAKE_LOCK_SUSPEND, "taos");
			wake_lock(&taos_wake_lock);
#endif

			if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x00), 0x00))) < 0) {
				printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x01), taos_cfgp->prox_int_time))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x02), taos_cfgp->prox_adc_time))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x03), taos_cfgp->prox_wait_time))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x0C), taos_cfgp->prox_intr_filter))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x0D), taos_cfgp->prox_config))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x0E), taos_cfgp->prox_pulse_cnt))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
                        if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x0F), taos_cfgp->prox_gain))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
			}
						
            		if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG|TAOS_TRITON_CMD_SPL_FN|TAOS_TRITON_CMD_PROX_INTCLR)))) < 0) {
                		printk(KERN_ERR "TAOS: i2c_smbus_write_byte failed in ioctl als_on\n");
                		return (ret);
            		}
			if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MAXTHRESHLO),taos_cfgp->prox_threshold_hi))) < 0) {    	       
				pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MAXTHRESHLO\n");            			      	 
			}     
			if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_PRX_MINTHRESHLO),taos_cfgp->prox_threshold_lo))) < 0) {    	       
				pr_crit(TAOS_TAG "i2c_smbus_write_byte() to TAOS_TRITON_PRX_MINTHRESHLO\n");            			      	 
			}    
            		if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x00), 0x2F))) < 0) { 
                		printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                		return (ret);
            		}
                 				#if defined(CONFIG_MACH_JOE)    
                        if((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x0C), 0x20))) < 0)
                        #else
                        if((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x0C), 0x30))) < 0)
                        #endif		 
                        {                    	
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_on\n");
                                return (ret);
                        }
            		prox_cur_infop->prox_event = 0;
            		prox_cur_infop->prox_clear = 0;
            		prox_cur_infop->prox_data = 0;
			pr_crit(TAOS_TAG "TAOS_IOCTL_PROX_ON\n");		
			
			prox_on = 1;
			break;
		case TAOS_IOCTL_PROX_OFF:
			if ((ret = (i2c_smbus_write_byte_data(taos_datap->client, (TAOS_TRITON_CMD_REG|0x00), 0x00))) < 0) {
                                printk(KERN_ERR "TAOS: i2c_smbus_write_byte_data failed in ioctl prox_off\n");
                                return (ret);
                        	}
			prox_on = 0;	
			pr_crit(TAOS_TAG "TAOS_IOCTL_PROX_OFF\n");
#ifdef CONFIG_PROX_WAKE_LOCK
			// destroy wake lock
			wake_lock_destroy(&taos_wake_lock);
			pr_crit(TAOS_TAG "release wake lock");
#endif
			break;
		case TAOS_IOCTL_PROX_DATA:
                        ret = copy_to_user((struct taos_prox_info *)arg, prox_cur_infop, sizeof(struct taos_prox_info));
                        if (ret) {
                                printk(KERN_ERR "TAOS: copy_to_user failed in ioctl prox_data\n");
                                return -ENODATA;
                        }
                        return (ret);
                        break;
                case TAOS_IOCTL_PROX_EVENT:
 			return (prox_cur_infop->prox_event);
                        break;
		case TAOS_IOCTL_PROX_CALIBRATE:
			if (!prox_on)			
			 {
				printk(KERN_ERR "TAOS: ioctl prox_calibrate was called before ioctl prox_on was called\n");
				return -EPERM;
			}
			prox_sum = 0;
			prox_max = 0;
			for (i = 0; i < 20; i++) {
	                        if ((ret = taos_prox_poll(&prox_cal_info[i])) < 0) {
        	                        printk(KERN_ERR "TAOS: call to prox_poll failed in ioctl prox_calibrate\n");
                	                return (ret);
                        	}		
				prox_sum += prox_cal_info[i].prox_data;
				if (prox_cal_info[i].prox_data > prox_max)
					prox_max = prox_cal_info[i].prox_data;
				mdelay(100);
			}
			prox_mean = prox_sum/20;
				taos_cfgp->prox_threshold_hi = prox_mean+1500;//13*prox_mean/10;
				taos_cfgp->prox_threshold_lo = prox_mean+1000;//12*prox_mean/10;
			if (taos_cfgp->prox_threshold_lo < ((sat_prox*12)/100)) {
				taos_cfgp->prox_threshold_lo = ((sat_prox*12)/100);
				taos_cfgp->prox_threshold_hi = ((sat_prox*15)/100);
			}
			
			pr_crit(KERN_ERR "taos prox_cal_threshold_hi=%d,prox_cal_threshold_lo=%d\n",taos_cfgp->prox_threshold_hi,taos_cfgp->prox_threshold_lo);
			break;
		default:
			return -EINVAL;
			break;
	}
	return (ret);
}

// read and calculate lux value
static int taos_get_lux(void) {
        u16 raw_clear = 0, raw_ir = 0, raw_lux = 0;
	u32 lux = 0;
	u32 ratio = 0;
	u8 dev_gain = 0;
	struct lux_data *p;
	int ret = 0;
	u8 chdata[4];
	int tmp = 0, i = 0;
    
	for (i = 0; i < 4; i++) {
	        if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | (TAOS_TRITON_ALS_CHAN0LO + i))))) < 0) {
        	        printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to chan0/1/lo/hi reg failed in taos_get_lux()\n");
                	return (ret);
        	}
        	chdata[i] = i2c_smbus_read_byte(taos_datap->client);
	}
	
	tmp = (taos_cfgp->als_time + 25)/50;
        TritonTime.numerator = 1;
        TritonTime.denominator = tmp;

        tmp = 300 * taos_cfgp->als_time;
        if(tmp > 65535)
                tmp = 65535;
        TritonTime.saturation = tmp;
	raw_clear = chdata[1];
	raw_clear <<= 8;
	raw_clear |= chdata[0];
	raw_ir    = chdata[3];
	raw_ir    <<= 8;
	raw_ir    |= chdata[2];
	
	als_intr_threshold_hi_param = raw_clear + raw_clear/5;
    	als_intr_threshold_lo_param = raw_clear - raw_clear/5;

	//update threshold 
    	if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_ALS_MAXTHRESHLO),als_intr_threshold_hi_param))) < 0) {
        	        printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to chan0/1/lo/hi reg failed in taos_get_lux()\n");
        	}
    	if ((ret = (i2c_smbus_write_word_data(taos_datap->client, (0xA0 | TAOS_TRITON_ALS_MINTHRESHLO),als_intr_threshold_lo_param))) < 0) {
        	        printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to chan0/1/lo/hi reg failed in taos_get_lux()\n");
        	}
	
	if(raw_ir > raw_clear) {
		raw_lux = raw_ir;
		raw_ir = raw_clear;
		raw_clear = raw_lux;
	}
	raw_clear *= taos_cfgp->scale_factor;	
	raw_ir *= taos_cfgp->scale_factor;
	dev_gain = taos_triton_gain_table[taos_cfgp->gain & 0x3];
        if(raw_clear >= lux_timep->saturation)
                return(TAOS_MAX_LUX);
        if(raw_ir >= lux_timep->saturation)
                return(TAOS_MAX_LUX);
        if(raw_clear == 0)
                return(0);
        if(dev_gain == 0 || dev_gain > 127) {
		printk(KERN_ERR "TAOS: dev_gain = 0 or > 127 in taos_get_lux()\n");
                return -1;
	}
        if(lux_timep->denominator == 0) {
		printk(KERN_ERR "TAOS: lux_timep->denominator = 0 in taos_get_lux()\n");
                return -1;
	}
	ratio = (raw_ir<<15)/raw_clear;
	for (p = lux_tablep; p->ratio && p->ratio < ratio; p++);
        if(!p->ratio)
                return 0;
	lux = ((raw_clear*(p->clear)) - (raw_ir*(p->ir)));
	lux = ((lux + (lux_timep->denominator >>1))/lux_timep->denominator) * lux_timep->numerator;
        lux = (lux + (dev_gain >> 1))/dev_gain;
	lux >>= TAOS_SCALE_MILLILUX;
        if(lux > TAOS_MAX_LUX)
                lux = TAOS_MAX_LUX;
	return(lux);
}

static int taos_lux_filter(int lux)
{
        static u8 middle[] = {1,0,2,0,0,2,0,1};
        int index;

        lux_history[2] = lux_history[1];
        lux_history[1] = lux_history[0];
        lux_history[0] = lux;
        if((lux_history[2] < 0) || (lux_history[1] < 0) || (lux_history[0] < 0))
		return -ENODATA;
        index = 0;
        if( lux_history[0] > lux_history[1] ) index += 4;
        if( lux_history[1] > lux_history[2] ) index += 2;
        if( lux_history[0] > lux_history[2] ) index++;
        return(lux_history[middle[index]]);
}

// verify device
static int taos_device_name(unsigned char *bufp, char **device_name) {
		if( (bufp[0x12]&0xf0)!=0x00)
			return(0);
		if(bufp[0x10]|bufp[0x1a]|bufp[0x1b]|bufp[0x1c]|bufp[0x1d]|bufp[0x1e])
			return(0);
  		if(bufp[0x13]&0x0c)
			return(0);
		*device_name="taos";
		return(1);
}

// proximity poll
static int taos_prox_poll(struct taos_prox_info *prxp) {
        int i = 0, ret = 0;//wait_count = 0
        u8 chdata[6];
        for (i = 0; i < 6; i++) {
                if ((ret = (i2c_smbus_write_byte(taos_datap->client, (TAOS_TRITON_CMD_REG | (TAOS_TRITON_ALS_CHAN0LO + i))))) < 0) {
                        printk(KERN_ERR "TAOS: i2c_smbus_write_byte() to als/prox data reg failed in taos_prox_poll()\n");
                        return (ret);
                }
                chdata[i] = i2c_smbus_read_byte(taos_datap->client);
        }
        prxp->prox_clear = chdata[1];
        prxp->prox_clear <<= 8;
        prxp->prox_clear |= chdata[0];
        if (prxp->prox_clear > ((sat_als*80)/100))
                return -ENODATA;
        prxp->prox_data = chdata[5];
        prxp->prox_data <<= 8;
        prxp->prox_data |= chdata[4];
        return (ret);
}


MODULE_AUTHOR("John Koshi - Surya Software");
MODULE_DESCRIPTION("TAOS ambient light and proximity sensor driver");
MODULE_LICENSE("GPL");

module_init(taos_init);
module_exit(taos_exit);

