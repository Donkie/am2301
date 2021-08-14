/*
 * am2301.c
 *
 * AM2301 (DHT21) Relative humidity and temperature [C].
 *
 * Usage:
 * 
 * insmod ./am2301.ko
 * cat /proc/am2301
 * 
 * Example output when measurement has been made successfully:
 *    43.2 RH, 25.1 C, ok
 * or if there are errors 
 *    43.2 RH, 25.1 C, error, checkum
 * or (no data, wrong pin or broken module) 
 *    0.0 RH, 0.0 C, error, no data
 * 
 * The module is using GPIO24 pin (Raspberry Pi B physical pin 18) by default..
 * GPIO pin can be changed when installing module:
 *    insmod ./am2301.ko gpio=22  # using GPIO22
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/irqflags.h>
#include <linux/spinlock.h>

// GPIO pin for input and output, GPIO24 
// note that physical connector pin number is different
static int gpio = 4;

// last measurement time, next measurement can not be done
// until measurement period has passed (see MEASUREMENT_PERIOD)
static long last_time = 0;

// minimum measurement period 2.0 seconds according to specifications
// driver will sleep until this period has passed
#define MEASUREMENT_PERIOD 2000 

// GPIO high (3.3V or 5V)
#define HIGH 1

// GPIO low (0V)
#define LOW 0

// Constant for indicating pulse waiting timeout
#define WAITTIMEOUT 0xFFFFFFFFFFFFFFFF

// Expect the signal line to be at the specified level for a period of time and
// return a count of loop cycles spent at that level (this cycle count can be
// used to compare the relative time of two pulses).  If more than a millisecond
// ellapses without the level changing then the call fails with a 0 response.
// This is adapted from Arduino's pulseInLong function (which is only available
// in the very latest IDE versions):
//   https://github.com/arduino/Arduino/blob/master/hardware/arduino/avr/cores/arduino/wiring_pulse.c
u64 expectPulse(int state) {
	u64 nsTime = 0;

	u64 start = ktime_get_real_ns();

	while (gpio_get_value(gpio) == state) {
		nsTime = ktime_get_real_ns() - start;
		if(nsTime > 1000000){
			return WAITTIMEOUT;
		}
	}

	return nsTime;
}

typedef struct {
	char msg[100];
	s32 temp;
	s32 humidity;
} ret;
static ret returnValue;

static void run(void)
{
	s32 temp, humidity;
	u8 data[5];
	u64 nstimes[80];
	int i;
	u64 lowCycles, highCycles;

	returnValue.temp = 0;
	returnValue.humidity = 0;

	// Go into high impedence state to let pull-up raise data line level and
	// start the reading process.
	gpio_direction_input(gpio);
	mdelay(1);

	// First set data line low for a period
	gpio_direction_output(gpio, 0);
	udelay(1100);

	{
		// End the start signal by setting data line high for 40 microseconds.
		gpio_direction_input(gpio);

		// Delay a moment to let sensor pull data line low.
		udelay(55);

		// Now start reading the data line to get the value from the DHT sensor.
		// Turn off interrupts temporarily because the next sections
		// are timing critical and we don't want any interruptions.
		local_irq_disable();

		// First expect a low signal for ~80 microseconds followed by a high signal
		// for ~80 microseconds again.
		if(expectPulse(LOW) == WAITTIMEOUT){
			strcpy(returnValue.msg, "error: timeout waiting for start signal low pulse");
			return;
		}

		if(expectPulse(HIGH) == WAITTIMEOUT){
			strcpy(returnValue.msg, "error: timeout waiting for start signal high pulse");
			return;
		}

		// Now read the 40 bits sent by the sensor.  Each bit is sent as a 50
		// microsecond low pulse followed by a variable length high pulse.  If the
		// high pulse is ~28 microseconds then it's a 0 and if it's ~70 microseconds
		// then it's a 1.  We measure the cycle count of the initial 50us low pulse
		// and use that to compare to the cycle count of the high pulse to determine
		// if the bit is a 0 (high state cycle count < low state cycle count), or a
		// 1 (high state cycle count > low state cycle count). Note that for speed
		// all the pulses are read into a array and then examined in a later step.
		for (i = 0; i < 80; i += 2) {
			nstimes[i] = expectPulse(LOW);
			nstimes[i + 1] = expectPulse(HIGH);
		}
	} // Timing critical code is now complete.
	local_irq_enable();

	// Inspect pulses and determine which ones are 0 (high state cycle count < low
	// state cycle count), or 1 (high state cycle count > low state cycle count).
	for (i = 0; i < 40; ++i) {
		lowCycles = nstimes[2 * i];
		highCycles = nstimes[2 * i + 1];
		if ((lowCycles == WAITTIMEOUT) || (highCycles == WAITTIMEOUT)) {
			strcpy(returnValue.msg, "error: timeout waiting for a pulse");
			return;
		}
		data[i / 8] <<= 1;
		// Now compare the low and high cycle times to see if the bit is a 0 or 1.
		if (highCycles > lowCycles) {
			// High cycles are greater than 50us low cycle count, must be a 1.
			data[i / 8] |= 1;
		}
		// Else high cycles are less than (or equal to, a weird case) the 50us low
		// cycle count so this must be a zero.  Nothing needs to be changed in the
		// stored data.
	}

	if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
		// Parse temp
		temp = ((u16)(data[2] & 0x7F)) << 8 | data[3];
		if (data[2] & 0x80) {
			temp = -temp;
		}

		// Parse humidity
		humidity = ((u16)data[0]) << 8 | data[1];

		returnValue.temp = temp;
		returnValue.humidity = humidity;
		strcpy(returnValue.msg, "ok");
		return;
	} else {
		strcpy(returnValue.msg, "error: checksum error");
		return;
	}
}

static int am2301_show(struct seq_file *m, void *v)
{
	s32 t, h;

	run();

	t = returnValue.temp;
	h = returnValue.humidity;

	seq_printf(m, "%d.%d RH, %d.%d C, %s\n", h/10, h%10, t/10, t%10, returnValue.msg);

	return 0;
}

static int am2301_open(struct inode *inode, struct  file *file) 
{
	long time_since_last_read = jiffies_to_msecs(jiffies - last_time);
  
	if (time_since_last_read >= 0 && time_since_last_read < MEASUREMENT_PERIOD) {
		msleep(MEASUREMENT_PERIOD - time_since_last_read);
	}
	last_time = jiffies;

	return single_open(file, am2301_show, NULL);
}

static struct proc_ops am2301_fops = {
    .proc_open = am2301_open,
    .proc_release = single_release,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek
};

static int __init am2301_init(void)
{
	int ret;

	printk(KERN_INFO "Initializing am2301 (dht21)\n");
	printk(KERN_INFO "Using GPIO%d\n", gpio);

	ret = gpio_request_one(gpio, GPIOF_OUT_INIT_HIGH, "AM2301");

	if (ret != 0) {
		printk(KERN_ERR "Unable to request GPIO, err: %d\n", ret);
		return ret;
	}

	proc_create_data("am2301", 0, NULL, &am2301_fops, NULL);
	return 0;
}

static void __exit am2301_exit(void)
{
 	(void) gpio_direction_output(gpio, 1);
	gpio_free(gpio);

	remove_proc_entry("am2301", NULL);
	printk(KERN_INFO "am2301 exit module\n");
}




module_init(am2301_init);
module_exit(am2301_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kari Aarnio");
MODULE_DESCRIPTION("AM2301 (DHT21) driver");

module_param(gpio, int, S_IRUGO);

MODULE_PARM_DESC(gpio, "Pin number for data input and output, assuming GPIO24 (Raspberry Model B physical pin #18)");

