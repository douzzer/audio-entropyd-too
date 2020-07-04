/*
 ** Simple program to reseed kernel random number generator using
 ** data read from soundcard.
 **
 ** Copyright 1999 Damien Miller <djm@mindrot.org>
 ** Copyright 2000-2009 by Folkert van Heusden <folkert@vanheusden.com>
 **
 ** This code is licensed under the GNU Public License version 2
 ** Please see the file COPYING for more details.
 **
 * *** empty log message ***
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sched.h>

#include <alsa/asoundlib.h>
#include <linux/soundcard.h>
#include <asm/types.h>
#include <linux/random.h>
#include <errno.h>

#include "proc.h"
#include "val.h"
#include "RNGTEST.h"
#include "error.h"

#include "aes.h"
#if AES_BLOCK_SIZE != 16
#error expecting compiled-in 128 bit AES.
#endif

#define RANDOM_DEVICE				"/dev/random"
#define DEFAULT_SAMPLE_RATE			11025
#define PID_FILE				"/var/run/audio-entropyd.pid"
#define DEFAULT_CLICK_READ			(1 * DEFAULT_SAMPLE_RATE)
#define DEFAULT_POOLSIZE_FN                     "/proc/sys/kernel/random/poolsize"
#define	RNGTEST_PENALTY				(20000 / 8) /* how many bytes to skip when the rng-test fails */

void dolog(int level, char *format, ...);

extern int loggingstate;
char skip_test = 0;
int error_state = 0;
char dofork = 1;
char *file = NULL;

static int got_mlockall = 0;

static int spike_mode = 0;
static double spike_threshold = 50;
static double spike_edge_min_delta = 20;
static uint32_t spike_channel_mask = 0x3;
#define SPIKE_ONSET_SAMPLE_DISCARD_MSBS 11
static size_t spike_minimum_interval_frames = 100;
static int spike_test_mode = 0;
#define SPIKE_IDLE_WARNING_SECONDS 60
static char *spike_log_path = 0;
static FILE *spike_log_file = 0;
static double spike_log_interval_seconds = 3600.0;

static char *cdevice = "hw:0";				/* capture device */
const char *id = "capture";
int err;
int verbose=0;
int format = -1;

#define max(x, y)	((x)>(y)?(x):(y))

/* Prototypes */
void main_loop(const char *cdevice, int sample_rate);
int setparams(snd_pcm_t *chandle, int sample_rate);
void usage(void);
void credit_krng(int random_fd, struct rand_pool_info *entropy);
void daemonise(void);
void gracefully_exit(int signum);
void logging_handler(int signum);
void get_random_data(int sample_rate, int skip_samples, int process_samples, int *n_output_bytes, char **output_buffer);
int add_to_kernel_entropyspool(int handle, char *buffer, int nbytes);

static void seed_continually_with_random_spike_data(int sample_rate, int skip_samples, int random_fd);

/* Functions */

int main(int argc, char **argv)
{
	int sample_rate = DEFAULT_SAMPLE_RATE;
	int c;
	static struct option long_options[] =
	{
		{"device",	1, NULL, 'd' },
		{"do-not-fork",	1, NULL, 'n' },
		{"sample-rate", 1, NULL, 'N' },
		{"spike-mode",  no_argument, 0, 'k' },
		{"spike-threshold-percent", required_argument, 0, 't' },
		{"spike-edge-min-delta-percent", required_argument, 0, 'T' },
		{"spike-channel-mask", required_argument, 0, 'c' },
		{"spike-minimum-interval-frames", required_argument, 0, 'i' },
		{"spike-test-mode", no_argument, 0, 256 },
		{"spike-log", required_argument, 0, 257 },
		{"spike-log-interval-seconds", required_argument, 0, 258 },
		{"skip-test",	0, NULL, 's' },
		{"file",	1, NULL, 'f' },
		{"verbose",	0, NULL, 'v' },
		{"help",	0, NULL, 'h' },
		{NULL,		0, NULL, 0   }
	};

	/* Process commandline options */
	while(1)
	{
		c = getopt_long (argc, argv, "f:nsr:d:N:kt:T:c:i:vh", long_options, NULL);
		if (c == -1)
			break;

		switch(c)
		{
			case 'f':
				file = optarg;
				break;

			case 'n':
				dofork = 0;
				break;

			case 'N':
				sample_rate = atoi(optarg);
				break;

			case 'k':
				spike_mode = 1;
				break;
			case 't': {
				char *cp;
				spike_threshold = strtod(optarg,&cp);
				if (*cp || (spike_threshold < 0) || (spike_threshold > 100)) {
					fprintf(stderr, "invalid threshold percentage \"%s\".\n",optarg);
					exit(1);
				}
				break;
			}
			case 'T': {
				char *cp;
				spike_edge_min_delta = strtod(optarg,&cp);
				if (*cp || (spike_edge_min_delta < 0) || (spike_edge_min_delta > 100)) {
					fprintf(stderr, "invalid spike-edge-min-delta-percent \"%s\".\n",optarg);
					exit(1);
				}
				break;
			}
			case 'c': {
				char *cp;
				spike_channel_mask = (int)strtoul(optarg, &cp, 0);
				if (*cp || (! (spike_channel_mask & 0x3))) {
					fprintf(stderr,"invalid spike detection channel mask \"%s\" -- must set at least one of bit 0 and bit 1.\n",optarg);
					exit(1);
				}
				break;
			}
			case 'i': {
				char *cp;
				spike_minimum_interval_frames = strtoul(optarg, &cp, 0);
				if (*cp) {
					fprintf(stderr,"invalid spike-minimum-interval-frames \"%s\".\n",optarg);
					exit(1);
				}
				break;
			}
			case 256:
				spike_test_mode = 1;
				break;
			case 257:
				spike_log_path = optarg;
				if (! (spike_log_file = fopen(spike_log_path,"a+"))) {
					perror(spike_log_path);
					exit(1);
				}
				break;
			case 258: {
				char *cp;
				spike_log_interval_seconds = strtod(optarg,&cp);
				if (*cp || (spike_log_interval_seconds < 0)) {
					fprintf(stderr, "invalid spike-log-interval-seconds \"%s\".\n",optarg);
					exit(1);
				}
				break;
			}
			case 'v':
				loggingstate = 1;
				verbose++;
				break;

			case 's':
				skip_test = 1;
				break;

			case 'd':
				cdevice = strdup(optarg);
				break;

			case 'h':
				usage();
				exit(0);

			case '?':
			default:
				fprintf(stderr, "Invalid commandline options.\n\n");
				usage();
				exit(1);
		}
	}

	RNGTEST_init();

	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, gracefully_exit);
	signal(SIGINT, gracefully_exit);
	signal(SIGTERM, gracefully_exit);
	signal(SIGUSR1, logging_handler);
	signal(SIGUSR2, logging_handler);

	openlog("audio-entropyd", LOG_CONS, LOG_DAEMON);

	dolog(LOG_INFO, "audio-entropyd starting up");

	if (mlockall(MCL_FUTURE | MCL_CURRENT) == -1)
		perror("mlockall");
	else
		got_mlockall = 1;

	static const struct sched_param sp = { .sched_priority = 1 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		perror("sched_setscheduler");

	if (dofork)
		daemonise();

	main_loop(cdevice, sample_rate);

	exit(0);
}

int setparams(snd_pcm_t *chandle, int sample_rate)
{
	snd_pcm_hw_params_t *ct_params;		/* templates with rate, format and channels */
	snd_pcm_hw_params_alloca(&ct_params);

	err = snd_pcm_hw_params_any(chandle, ct_params);
	if (err < 0)
		error_exit("Broken configuration for %s PCM: no configurations available: %s", id, snd_strerror(err));

	/* Disable rate resampling */
	err = snd_pcm_hw_params_set_rate_resample(chandle, ct_params, 0);
	if (err < 0)
		error_exit("Could not disable rate resampling: %s", snd_strerror(err));

	/* Set access to SND_PCM_ACCESS_RW_INTERLEAVED */
	err = snd_pcm_hw_params_set_access(chandle, ct_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
		error_exit("Could not set access to SND_PCM_ACCESS_RW_INTERLEAVED: %s", snd_strerror(err));

	/* Restrict a configuration space to have rate nearest to our target rate */
	err = snd_pcm_hw_params_set_rate_near(chandle, ct_params, &sample_rate, 0);
	if (err < 0)
		error_exit("Rate %iHz not available for %s: %s", sample_rate, id, snd_strerror(err));

	/* Set sample format */
	format = SND_PCM_FORMAT_S16_LE;
	err = snd_pcm_hw_params_set_format(chandle, ct_params, format);
	if (err < 0)
	{
		format = SND_PCM_FORMAT_S16_BE;
		err = snd_pcm_hw_params_set_format(chandle, ct_params, format);
	}
	if (err < 0)
		error_exit("Sample format (SND_PCM_FORMAT_S16_BE and _LE) not available for %s: %s", id, snd_strerror(err));

	/* Set stereo */
	err = snd_pcm_hw_params_set_channels(chandle, ct_params, 2);
	if (err < 0)
		error_exit("Channels count (%i) not available for %s: %s", 2, id, snd_strerror(err));

	{
	  snd_pcm_uframes_t buf_sz = 1L<<20L;
	  if ((err = snd_pcm_hw_params_set_buffer_size_max(chandle, ct_params, &buf_sz)) < 0)
	    error_exit("buf sz not settable for %s: %s", id, snd_strerror(err));
	}

	/* Apply settings to sound device */
	err = snd_pcm_hw_params(chandle, ct_params);
	if (err < 0)
		error_exit("Could not apply settings to sound device: %s", snd_strerror(err));

	return 0;
}

void main_loop(const char *cdevice, int sample_rate)
{
	unsigned char *output_buffer = NULL;
	int n_output_bytes = -1;
	int random_fd = -1, max_bits;
	FILE *poolsize_fh;

	/* Open kernel random device */
	random_fd = open(RANDOM_DEVICE, O_RDWR);
	if (random_fd == -1)
		error_exit("Couldn't open random device: %m");

	/* find out poolsize */
	poolsize_fh = fopen(DEFAULT_POOLSIZE_FN, "rb");
	if (!poolsize_fh)
		error_exit("Couldn't open poolsize file: %m");
	fscanf(poolsize_fh, "%d", &max_bits);
	fclose(poolsize_fh);

	if (spike_mode) {
		seed_continually_with_random_spike_data(sample_rate, DEFAULT_CLICK_READ, random_fd);
		__builtin_unreachable();
		return;
	}

	/* first get some data so that we can immediately submit something when the
	 * kernel entropy-buffer gets below some limit
	 */
	get_random_data(sample_rate, DEFAULT_CLICK_READ, DEFAULT_SAMPLE_RATE, &n_output_bytes, &output_buffer);

	/* Main read loop */
	for(;;)
	{	
		int added = 0, before, loop, after;

		if (!file)
		{
			fd_set write_fd;
			FD_ZERO(&write_fd);
			FD_SET(random_fd, &write_fd);
			for(;;) 
			{ 
				int rc = select(random_fd+1, NULL, &write_fd, NULL, NULL); /* wait for krng */ 
				if (rc >= 0) break; 
				if (errno != EINTR) 
					error_exit("Select error: %m"); 
			}

			/* find out how many bits to add */
			if (ioctl(random_fd, RNDGETENTCNT, &before) == -1)
				error_exit("Couldn't query entropy-level from kernel");

			dolog(LOG_DEBUG, "woke up due to low entropy state (%d bits left)", before);
		}

		/* loop until the buffer is (supposed to be) full: we do NOT check the number of bits
		 * currently in the buffer each iteration, since (on a heavily used random-driver)
		 * audio-entropyd might run constantly, using a lot of cpu-usage
		 */
		if (verbose > 1)
			printf("max_bits: %d\n", max_bits);
		for(loop=0; loop < max_bits;)
		{
			if (verbose > 1)
				dolog(LOG_DEBUG, "n_output_bytes: %d", n_output_bytes);

			if (n_output_bytes > 0)
			{
				int cur_added;

				if (file)
				{
					FILE *fh = fopen(file, "a+");
					if (!fh)
						error_exit("error accessing file %s", file);

					if (fwrite(output_buffer, 1, n_output_bytes, fh) != n_output_bytes)
						error_exit("error writeing to file");

					fclose(fh);

					cur_added = n_output_bytes * 8;
				}
				else
				{
					cur_added = add_to_kernel_entropyspool(random_fd, output_buffer, n_output_bytes);
				}

				added += cur_added;
				loop += cur_added;

				if (verbose > 1)
					dolog(LOG_DEBUG, "%d bits of data, %d bits usable were added, total %d added", n_output_bytes * 8, cur_added, added);
			}

			if (! file) {
				/* Get number of bits in KRNG after credit */
				if (ioctl(random_fd, RNDGETENTCNT, &after) == -1)
					error_exit("Coundn't query entropy-level from kernel: %m");

				if (verbose > 1 && after < max_bits)
					dolog(LOG_DEBUG, "minimum level not reached: %d", after);
			}

			free(output_buffer);
			output_buffer = NULL;
			get_random_data(sample_rate, DEFAULT_CLICK_READ, DEFAULT_SAMPLE_RATE, &n_output_bytes, &output_buffer);
		}

		if (! file)
			dolog(LOG_INFO, "Entropy credit of %i bits made (%i bits before, %i bits after)", added, before, after);
	}
}

int add_to_kernel_entropyspool(int handle, char *buffer, int nbytes)
{
	double nbits;
	struct rand_pool_info *output;

	output = (struct rand_pool_info *)malloc(sizeof(struct rand_pool_info) + nbytes);
	if (!output)
		error_exit("malloc failure in add_to_kernel_entropyspool");

	// calculate number of bits in the block of
	// data. put in structure
	nbits = calc_nbits_in_data((unsigned char *)buffer, nbytes);
	if (nbits >= 1.0)
	{
		output -> entropy_count = (int)nbits;
		output -> buf_size      = nbytes;
		memcpy(output -> buf, buffer, nbytes);

		if (ioctl(handle, RNDADDENTROPY, output) == -1)
			error_exit("RNDADDENTROPY failed!");
	}

	free(output);

	return (int)nbits;
}

#define order(a, b)     (((a) == (b)) ? -1 : (((a) > (b)) ? 1 : 0))

void get_random_data(int sample_rate, int skip_samples, int process_samples, int *n_output_bytes, char **output_buffer)
{
	int n_to_do, bits_out=0, loop;
	char *dummy;
	static short psl=0, psr=0; /* previous samples */
	static char a=1; /* alternater */
	unsigned char byte_out=0;
	int input_buffer_size;
	char *input_buffer;
	snd_pcm_t *chandle;

	if (verbose > 1)
		dolog(LOG_DEBUG, "get_random_data(%p, %d, %d, %p, %p)", chandle, skip_samples, process_samples, n_output_bytes, output_buffer);

	if ((err = snd_pcm_open(&chandle, cdevice, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		error_exit("Record open error: %s", snd_strerror(err));

	/* Open and set up ALSA device for reading */
	setparams(chandle, sample_rate);

	*n_output_bytes=0;

	input_buffer_size = snd_pcm_frames_to_bytes(chandle, max(skip_samples, process_samples)) * 2; /* *2: stereo! */
	input_buffer = (char *)malloc(input_buffer_size);
	*output_buffer = (char *)malloc(input_buffer_size);
	if (!input_buffer || !output_buffer)
		error_exit("problem allocating %d bytes of memory", input_buffer_size);
	if (verbose > 1)
		dolog(LOG_DEBUG, "Input buffer size: %d bytes", input_buffer_size);

	/* Discard the first data read */
	/* it often contains weird looking data - probably a click from */
	/* driver loading / card initialisation */
	snd_pcm_sframes_t garbage_frames_read = snd_pcm_readi(chandle, input_buffer, skip_samples);
	/* Make sure we aren't hitting a disconnect/suspend case */
	if (garbage_frames_read < 0)
		snd_pcm_recover(chandle, garbage_frames_read, 0);
	/* Nope, something else is wrong. Bail. */
	if (garbage_frames_read < 0)
		error_exit("Get random data: read error: %m");

	/* Read a buffer of audio */
	n_to_do = process_samples * 2;
	dummy = input_buffer;
	while (n_to_do > 0)
	{
		snd_pcm_sframes_t frames_read = snd_pcm_readi(chandle, dummy, n_to_do);
		/* Make	sure we	aren't hitting a disconnect/suspend case */
		if (frames_read < 0)
			frames_read = snd_pcm_recover(chandle, frames_read, 0);
		/* Nope, something else is wrong. Bail.	*/
		if (frames_read < 0)
			error_exit("Read error: %m");
		if (frames_read == -1) 
		{
			if (errno != EINTR)
				error_exit("Read error: %m");
		}
		else
		{
			n_to_do -= frames_read;
			dummy += frames_read;	
		}
	}
	snd_pcm_close(chandle);

	/* de-biase the data */
	for(loop=0; loop<(process_samples * 2/*16bits*/ * 2/*stereo*/ * 2); loop+=8)
	{
		int w1, w2, w3, w4, o1, o2;

		if (format == SND_PCM_FORMAT_S16_BE)
		{
			w1 = (input_buffer[loop+0]<<8) + input_buffer[loop+1];
			w2 = (input_buffer[loop+2]<<8) + input_buffer[loop+3];
			w3 = (input_buffer[loop+4]<<8) + input_buffer[loop+5];
			w4 = (input_buffer[loop+6]<<8) + input_buffer[loop+7];
		}
		else
		{
			w1 = (input_buffer[loop+1]<<8) + input_buffer[loop+0];
			w2 = (input_buffer[loop+3]<<8) + input_buffer[loop+2];
			w3 = (input_buffer[loop+5]<<8) + input_buffer[loop+4];
			w4 = (input_buffer[loop+7]<<8) + input_buffer[loop+6];
		}

		/* Determine order of channels for each sample, subtract previous sample
		 * to compensate for unbalanced audio devices */
		o1 = order(w1-psl, w2-psr);
		o2 = order(w3-psl, w4-psr);
		if (a > 0)
		{
			psl = w3;
			psr = w4;
		}
		else
		{
			psl = w1;
			psr = w2;
		}

		/* If both samples have the same order, there is bias in the samples, so we
		 * discard them; if both channels are equal on either sample, we discard
		 * them too; additionally, alternate the sample we'll use next (even more
		 * bias removal) */
		if (o1 == o2 || o1 < 0 || o2 < 0)
		{
			a = -a;
		}
		else
		{
			/* We've got a random bit; the bit is either the order from the first or
			 * the second sample, determined by the alternator 'a' */
			char bit = (a > 0) ? o1 : o2;

			byte_out <<= 1;
			byte_out += bit;

			bits_out++;

			if (bits_out>=8)
			{
				if (error_state == 0 || skip_test == 0)
				{
					(*output_buffer)[*n_output_bytes]=byte_out;
					(*n_output_bytes)++;
				}
				bits_out=0;

				RNGTEST_add(byte_out);
				if (skip_test == 0 && RNGTEST() == -1)
				{
					if (error_state == 0)
						dolog(LOG_CRIT, "test of random data failed, skipping %d bytes before re-using data-stream (%d bytes in flush)", RNGTEST_PENALTY, error_state);
					error_state = RNGTEST_PENALTY;
					*n_output_bytes = 0;
				}
				else
				{
					if (error_state > 0)
					{
						error_state--;

						if (error_state == 0)
							dolog(LOG_INFO, "Restarting fetching of entropy data");
					}
				}
			}
		}
	}

	if (verbose > 1)
		dolog(LOG_DEBUG, "get_random_data() finished");

	free(input_buffer);
}

static void seed_continually_with_random_spike_data(int sample_rate, int skip_samples, int random_fd) {
	size_t cur_sample_number = 0;
	ssize_t last_spike_at[2] = {};
	size_t last_sample_number_first_order_delta[2] = {};
	int prev_sample[2] = {}, prev_spike_prev_sample[2] = {};
	size_t last_idle_warning_at = 0;
	size_t idle_warning_n_samples = SPIKE_IDLE_WARNING_SECONDS * (size_t)sample_rate;

	size_t spike_log_interval_samples = (size_t)round(spike_log_interval_seconds * (double)sample_rate);
	size_t next_log_at = spike_log_interval_samples;
	size_t log_cum_counts[2] = {};
	long double log_cum_ISI_hz[2] = {};

	unsigned __int128 collected_entropy = 0, last_collected_entropy = 0;
	int n_bits_of_collected_entropy = 0;

	struct rand_pool_info *output = (struct rand_pool_info *)malloc(sizeof(struct rand_pool_info) + sizeof collected_entropy);
	if (! output)
		error_exit("malloc failure in %s",__FUNCTION__);

	FILE *raw_out_file = 0;
	if (file) {
		raw_out_file = fopen(file, "a+");
		if (! raw_out_file)
			error_exit("error accessing file %s", file);
	}

	void maybe_reopen_raw_out_file(void) {
		struct stat st;
		if ((stat(file, &st) < 0) ||
		    (ftell(raw_out_file) > st.st_size)) {
			(void)fclose(raw_out_file);
			if (! (raw_out_file = fopen(file,"a+"))) {
				perror(file);
				return;
			}
		}
	}

	int input_buffer_size;
	char *input_buffer;
	snd_pcm_t *chandle;

	if ((err = snd_pcm_open(&chandle, cdevice, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		error_exit("Record open error: %s", snd_strerror(err));

	/* Open and set up ALSA device for reading */
	setparams(chandle, sample_rate);

	int spike_threshold_int = (int)((spike_threshold / 100.0) * 32767.0);
	if (spike_threshold < 0)
		spike_threshold_int = -spike_threshold_int;
	int spike_edge_min_delta_int = (spike_edge_min_delta / 100.0) * 32767.0;
	int spike_onset_sample_retained_bits = (sizeof(int) * 8UL) - __builtin_clz(spike_threshold_int) + 1UL - SPIKE_ONSET_SAMPLE_DISCARD_MSBS;

	int process_samples = sample_rate / 4;

	input_buffer_size = snd_pcm_frames_to_bytes(chandle, max(process_samples, skip_samples)) * 2; /* *2: stereo! */
	input_buffer = (char *)malloc(input_buffer_size);
	if (! input_buffer)
		error_exit("problem allocating %d bytes of memory", input_buffer_size);
	if (verbose > 1)
		dolog(LOG_DEBUG, "Input buffer size: %d bytes", input_buffer_size);

	/* Discard the first data read */
	/* it often contains weird looking data - probably a click from */
	/* driver loading / card initialisation */
	snd_pcm_sframes_t garbage_frames_read = snd_pcm_readi(chandle, input_buffer, skip_samples);
	/* Make sure we aren't hitting a disconnect/suspend case */
	if (garbage_frames_read < 0)
		snd_pcm_recover(chandle, garbage_frames_read, 0);
	/* Nope, something else is wrong. Bail. */
	if (garbage_frames_read < 0)
		error_exit("Get random data: read error: %m");

	void __attribute__((format(printf,1,2))) post_to_spike_log_file(const char *fmt,...) {
		if (! spike_log_file)
			return;
		struct stat st;
		if ((stat(spike_log_path, &st) < 0) ||
		    (ftell(spike_log_file) > st.st_size)) {
			(void)fclose(spike_log_file);
			if (! (spike_log_file = fopen(spike_log_path,"a+"))) {
				perror(spike_log_path);
				return;
			}
		}
		{
			struct timespec now;
			clock_gettime(CLOCK_REALTIME, &now);
			struct tm now_tm;
			gmtime_r(&now.tv_sec, &now_tm);
			char datebuf[40];
			size_t datelen = strftime(datebuf, sizeof datebuf, "%Y-%m-%dT%H:%M:%S", &now_tm);
			fprintf(spike_log_file, "%.*s.%06uZ ", (int)datelen, datebuf, (unsigned)round((double)now.tv_nsec / 1000.0));
		}
		va_list ap;
		va_start(ap,fmt);
		vfprintf(spike_log_file,fmt,ap);
		fflush(spike_log_file);
	}

	if (spike_log_file)
		post_to_spike_log_file("STARTUP\n");

	size_t total_popcount = 0, last_total_popcount = 0;
	size_t total_retained_bits = 0, last_total_retained_bits = 0;

	size_t total_byte_sum = 0, last_total_byte_sum = 0;
	size_t total_byte_sum_denom = 0, last_total_byte_sum_denom = 0;

	size_t n_all_ones = 0, n_all_zeros = 0;

	size_t total_events = 0, last_total_events = 0;
	size_t last_cur_sample_number = 0;

	size_t *chisquare_bins = calloc((1UL << 8UL),sizeof(*chisquare_bins));
	if (! chisquare_bins)
		error_exit("chisquare_bins = calloc(%zu,%zu): %m",(1UL << 8UL),sizeof(*chisquare_bins));

	aes_context aes_ctx = {};

	for (;;) {
		if ((cur_sample_number - last_spike_at[0] > idle_warning_n_samples) &&
		    (cur_sample_number - last_spike_at[1] > idle_warning_n_samples)) {
			if (! last_idle_warning_at) {
				last_idle_warning_at = cur_sample_number;
				dolog(LOG_ERR, "no spikes detected in %d seconds.", SPIKE_IDLE_WARNING_SECONDS);
				if (spike_log_file)
					post_to_spike_log_file("OUTAGE -- no spikes for %d s.\n", SPIKE_IDLE_WARNING_SECONDS);
			}
		} else {
			if (last_idle_warning_at) {
				double outage_duration = ((double)(cur_sample_number - last_idle_warning_at) / (double)sample_rate) + (double)SPIKE_IDLE_WARNING_SECONDS;
				if (spike_log_file)
					post_to_spike_log_file("RESUMED -- spike(s) detected after %.1f s outage.\n", outage_duration);
				dolog(LOG_ERR, "spikes resumed after %.1f second outage.", outage_duration);
				last_idle_warning_at = 0;
			}
		}

		if (spike_log_file && (cur_sample_number >= next_log_at)) { /* because of lumpiness in the reading, there will be jitter here. */
			next_log_at += spike_log_interval_samples;

			double chisquare_score = 0; /* (𝚺(x_i^2 / m_i)) - n */
			for (size_t i = 0; i < (1UL << 8UL); ++i) {
				double x = (double)chisquare_bins[i];
				chisquare_score += (x*x);
			}
			{
				double m = (double)total_byte_sum_denom / (double)(1UL << 8UL);
				chisquare_score /= m;
			}
			chisquare_score -= (double)total_byte_sum_denom;
			double chisquare_median = 1.0 - (2.0 / (9.0 * (double)(1UL << 8UL))); /* approximation per https://en.wikipedia.org/wiki/Chi-squared_distribution */
			chisquare_median = (double)(1UL << 8UL) * chisquare_median * chisquare_median * chisquare_median;
			const double chisquare_sd = sqrt(2.0 * (double)(1UL << 8UL));

			post_to_spike_log_file("N%s%.*lu%s%.*lu C/sd=%+.1f E=%zu B=%.3f%% Bcum=%.6f%% Bcum/sd=%+.1f A=%.1f Acum=%.3f Acum/sd=%+.1f ChiSq=%.2f ChiSq/sd=%+.1f n=%zu z=%zu o=%zu m_hz=%.2Lf brst=%.2Lf\n",
					       (spike_channel_mask & 0x1) ? " C0=" : "",
					       (spike_channel_mask & 0x1) ? 1 : 0,
					       log_cum_counts[0],
					       (spike_channel_mask & 0x2) ? " C1=" : "",
					       (spike_channel_mask & 0x2) ? 1 : 0,
					       log_cum_counts[1],
					       (((double)(cur_sample_number - last_cur_sample_number) / (double)sample_rate) *
						(((double)(total_events - last_total_events) / ((double)(cur_sample_number - last_cur_sample_number) / (double)sample_rate))
						 - ((double)total_events / ((double)cur_sample_number / (double)sample_rate))))
					       / sqrt(((double)(cur_sample_number - last_cur_sample_number) / (double)sample_rate)
						      * (double)total_events / ((double)cur_sample_number / (double)sample_rate)), /* Poisson dist */
					       total_retained_bits - last_total_retained_bits,
					       ((total_retained_bits > last_total_retained_bits) ?
						100.0 * (double)(total_popcount - last_total_popcount) / (double)(total_retained_bits - last_total_retained_bits) :
						-1),
					       100.0 * (double)total_popcount / (double)total_retained_bits,
					       ((double)total_popcount - ((double)total_retained_bits * 0.5)) / sqrt(0.25 * (double)total_retained_bits), /* binomial dist */
					       ((total_byte_sum > last_total_byte_sum) ?
						(double)(total_byte_sum - last_total_byte_sum) / (double)(total_byte_sum_denom - last_total_byte_sum_denom) :
						-1),
					       (double)total_byte_sum / (double)total_byte_sum_denom,
					       (((double)total_byte_sum / 255.0) - ((double)total_byte_sum_denom * 0.5)) / sqrt((double)total_byte_sum_denom / 12.0),  /* Irwin-Hall dist */
					       chisquare_score,
					       (chisquare_score - chisquare_median) / chisquare_sd,
					       total_byte_sum_denom, n_all_zeros, n_all_ones,
					       /* avg(1/ISI) */
					       (log_cum_ISI_hz[0] + log_cum_ISI_hz[1]) / (long double)(total_events - last_total_events),
					       /* burstiness metric: avg(1/ISI), normalized by 1/avg(ISI), minus 1 */
					       (((log_cum_ISI_hz[0] + log_cum_ISI_hz[1]) / (long double)(total_events - last_total_events))
						/ ((long double)(total_events - last_total_events) /
						   ((long double)(cur_sample_number - last_cur_sample_number) / (long double)sample_rate)))
					       - 1.0l
				);
			log_cum_counts[0] = log_cum_counts[1] = 0;
			log_cum_ISI_hz[0] = log_cum_ISI_hz[1] = 0.0l;
			last_total_events = total_events;
			last_cur_sample_number = cur_sample_number;
			last_total_popcount = total_popcount;
			last_total_retained_bits = total_retained_bits;
			last_total_byte_sum = total_byte_sum;
			last_total_byte_sum_denom = total_byte_sum_denom;
		}

		snd_pcm_sframes_t frames_read = snd_pcm_readi(chandle, input_buffer, process_samples * 2);
		/* Make	sure we	aren't hitting a disconnect/suspend case */
		if (frames_read < 0)
			frames_read = snd_pcm_recover(chandle, frames_read, 0);
		/* Nope, something else is wrong. Bail.	*/
		if (frames_read < 0)
			error_exit("Read error: %m");
		if (frames_read == -1) 
		{
			if (errno != EINTR)
				error_exit("Read error: %m");
		}

#ifndef min
#define min(x,y) ({ typeof(x) _x = (x); typeof(y) _y = (y); (_x < _y) ? _x : _y; })
#endif

		for(int loop=0; loop<(frames_read * 2/*16bits*/ * 2/*stereo*/); loop+=4, ++cur_sample_number) {
			for (int channel = 0; channel < 2; ++channel) {
				if (! (spike_channel_mask & (1 << channel)))
					continue;

				int word;
				if (format == SND_PCM_FORMAT_S16_LE)
					word = (int)*(short int *)(input_buffer + loop + (channel * 2));
				else
					word = (int)__builtin_bswap16(*(short int *)(input_buffer + loop + (channel * 2)));

				if (spike_threshold < 0)
					word = -word;

				if ((word > spike_threshold_int) &&
				    (prev_sample[channel] < spike_threshold_int) &&
				    (word - prev_sample[channel] > spike_edge_min_delta_int) &&
				    (cur_sample_number - last_spike_at[channel] >= spike_minimum_interval_frames)) {
					++total_events;
					size_t sample_number_first_order_delta = cur_sample_number - last_spike_at[channel];
					last_spike_at[channel] = cur_sample_number;
					/* have to choose the number of bits from the first order delta,
					 * because if it's taken directly from the second order delta,
					 * that biases against runs of leading zeros in the latter,
					 * which of course naturally occur.
					 */
					int n_sample_number_bits =
						(int)(sizeof sample_number_first_order_delta * 8UL)
						- (last_sample_number_first_order_delta[channel] ?
						   (int)min(__builtin_clzl(sample_number_first_order_delta),
						       __builtin_clzl(last_sample_number_first_order_delta[channel])) :
						   (int)__builtin_clzl(sample_number_first_order_delta))
						- 4;
					if (n_sample_number_bits <= 0)
						n_sample_number_bits = 1;
					ssize_t sample_number_second_order_delta = (ssize_t)sample_number_first_order_delta - (ssize_t)last_sample_number_first_order_delta[channel];
					last_sample_number_first_order_delta[channel] = sample_number_first_order_delta;

#if 0
					/* the sign bit is correlated, because the second order delta can't monotonically shrink or grow. */
					/* always retain the sign bit, by moving it to the lsb. */
					if (sample_number_second_order_delta < 0)
						sample_number_second_order_delta = (sample_number_second_order_delta << 1UL) | 1UL;
					else
						sample_number_second_order_delta <<= 1UL;
#endif

					/* get some phase information from the last below-threshold sample --
					 * with the soundcard at 192k, and given a leading edge slew rate around
					 * half of full scale for consecutive samples, suggests the lsb is sensitive
					 * to perturbations under 1 ns (1 / (32767 * 192000) = 159 ps).
					 * moving the sign bit to the lsb further aids sensitivity.
					 *
					 * technically this calls for sinc() interpolation, but that's overkill
					 * for present purposes.
					 */
					int delta_of_prev_sample = prev_sample[channel] - prev_spike_prev_sample[channel];
					prev_spike_prev_sample[channel] = prev_sample[channel];

#if 0
					/* the sign bit is correlated, because the prev_sample can't monotonically shrink or grow. */
					if (delta_of_prev_sample < 0)
						delta_of_prev_sample = (delta_of_prev_sample << 1) | 1;
					else
						delta_of_prev_sample <<= 1;
#endif

					ssize_t bits =
						(sample_number_second_order_delta << spike_onset_sample_retained_bits) |
						((size_t)delta_of_prev_sample & ((1UL << spike_onset_sample_retained_bits) - 1UL));

//					unsigned n_bits = (sizeof bits * 8UL) - __builtin_clzl((bits > 0) ? bits : -bits);
//					++n_bits; /* keep the sign bit. */

					unsigned n_bits = (unsigned)n_sample_number_bits + spike_onset_sample_retained_bits;

					if (spike_test_mode)
						printf("%zd 0x%zx bits=%u(=%u+%u) 1st=%zu 2nd=%zd prev=%d this=%d prev_delta=%d (0x%lx, %d bit%s)\n",bits,bits & ((1UL << n_bits) - 1UL), n_bits, n_sample_number_bits, spike_onset_sample_retained_bits, sample_number_first_order_delta, sample_number_second_order_delta, prev_sample[channel], word, delta_of_prev_sample, ((size_t)delta_of_prev_sample & ((1UL << (size_t)spike_onset_sample_retained_bits) - 1UL)), spike_onset_sample_retained_bits, spike_onset_sample_retained_bits == 1 ? "" : "s");

					++log_cum_counts[channel];
					log_cum_ISI_hz[channel] += (long double)sample_rate / (long double)sample_number_first_order_delta;

					total_popcount += __builtin_popcountl(bits & ((1UL << n_bits) - 1UL));
					total_retained_bits += n_bits;

					int unused_bits = 0;
					if (n_bits_of_collected_entropy + n_bits > (sizeof(collected_entropy) * 8UL)) {
						unused_bits = (n_bits_of_collected_entropy + n_bits) - (sizeof(collected_entropy) * 8UL);
						n_bits -= unused_bits;
					}

					collected_entropy <<= n_bits;
					collected_entropy |= ((bits >> unused_bits) & ((1UL << n_bits) - 1UL));
					n_bits_of_collected_entropy += n_bits;
					if (n_bits_of_collected_entropy >= (sizeof(collected_entropy) * 8UL)) {
						size_t this_byte_sum = 0;
						for (size_t b=0; b<sizeof collected_entropy * 8UL; b += 8UL) {
							size_t this_byte = (size_t)(collected_entropy >> b) & 0xffUL;
							this_byte_sum += this_byte;
							++chisquare_bins[this_byte];
							if (this_byte == 0xffUL)
								++n_all_ones;
							else if (this_byte == 0x0UL)
								++n_all_zeros;
						}
						total_byte_sum += this_byte_sum;
						total_byte_sum_denom += sizeof collected_entropy;
						int popcount = __builtin_popcountl((unsigned long)collected_entropy) + __builtin_popcountl((unsigned long)(collected_entropy >> 64UL));
						if (spike_test_mode) {
							double avg = (double)this_byte_sum / (double)sizeof collected_entropy;
							printf("emitting %d bits, popcount %d, avg %.1f, %d bit%s left over; Bcum %f%% (%+.1fsd), Acum %.3f (%+.1fsd))\n",
							       n_bits_of_collected_entropy,
							       popcount,
							       avg,
							       unused_bits,
							       unused_bits == 1 ? "" : "s",
							       100.0 * (double)total_popcount / (double)total_retained_bits,
							       ((double)total_popcount - ((double)total_retained_bits * 0.5)) / sqrt(0.25 * (double)total_retained_bits),
							       (double)total_byte_sum / (double)total_byte_sum_denom,
							       (((double)total_byte_sum / 255.0) - ((double)total_byte_sum_denom * 0.5)) / sqrt((double)total_byte_sum_denom / 12.0)  /* Irwin-Hall dist */
								);
						}

						/* set an AES key with random data, then discard the data. */
						if (! aes_ctx.aes_Nkey) {
							aes_set_key(&aes_ctx, (const unsigned char *)&collected_entropy, (int)sizeof collected_entropy, 0);
							goto skip_writing;
						}
						/* set an IV with random data, then discard the data. */
						if (! last_collected_entropy) {
							last_collected_entropy = collected_entropy;
							goto skip_writing;
						}

						if (raw_out_file)
							maybe_reopen_raw_out_file();
						if (raw_out_file) {
							/*
							 * write out the raw entropy with no whitening at all, for cryptoanalytic evaluation.
							 *
							 * do cursory evaluation of output file using an entropy analyzer, e.g.
							 * http://www.fourmilab.ch/random/
							 * or
							 * http://webhome.phy.duke.edu/~rgb/General/dieharder.php
							 */
							if (fwrite(&collected_entropy, 1UL, sizeof collected_entropy, raw_out_file) != sizeof collected_entropy) {
								dolog(LOG_CRIT, "%s: %m", file);
								(void)fclose(raw_out_file);
								raw_out_file = 0;
							} else
								fflush(raw_out_file);
						}
						if (! spike_test_mode) {
							/* CBC mode with random key and IV set above. */
							collected_entropy ^= last_collected_entropy;
							aes_encrypt(&aes_ctx, (const unsigned char *)&collected_entropy, (unsigned char *)output->buf);
							output->entropy_count = (int)(sizeof collected_entropy * 8UL);
							output->buf_size      = (int)sizeof collected_entropy;
							if (ioctl(random_fd, RNDADDENTROPY, output) < 0)
								error_exit("RNDADDENTROPY for fd %d failed in %s!",random_fd,__FUNCTION__);
							/* why RNDADDENTROPY doesn't credit it is a mystery, but a fact... */
							if (ioctl(random_fd, RNDADDTOENTCNT, &output->entropy_count) < 0)
								error_exit("RNDADDTOENTCNT %d for fd %d failed in %s!",output->entropy_count,random_fd,__FUNCTION__);
						}

						last_collected_entropy = collected_entropy;

					skip_writing:
						collected_entropy = bits;
						n_bits_of_collected_entropy = unused_bits;
					}
				}
				prev_sample[channel] = word;
			}
		}
	}
	__builtin_unreachable();
}

void usage(void)
{
	fprintf(stderr, "Usage: audio-entropyd [options]\n\n");
	fprintf(stderr, "Collect entropy from a soundcard and feed it into the kernel random pool.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "--device,       -d []  Specify sound device to use. (Default %s)\n", cdevice);
	fprintf(stderr, "--sample-rate,  -N []  Audio sampling rate. (default %i)\n", DEFAULT_SAMPLE_RATE);

	fprintf(stderr, "--spike-mode,   -k     Continually search for spikes (typically from a Geiger counter) and seed from inter-spike interval\n");
	fprintf(stderr, "--spike-threshold-percent, -t []  Threshold for spike detection, negative for negative-going spikes\n");
	fprintf(stderr, "--spike-edge-min-delta-percent, -T []  Minimum change in consecutive sample value for an above-threshold sample to qualify as a spike onset\n");
	fprintf(stderr, "--spike-channel-mask, -c []  Mask of channels to search for spikes in, bitwise-or of 1 for channel zero, 2 for channel one\n");
	fprintf(stderr, "--spike-minimum-interval-frames, -i []  Reject spikes closer than this many raw frames apart (relative to requested sample rate)\n");
	fprintf(stderr, "--spike-test-mode      Run spike mode for testing -- print events, and don't add entropy to the entropy pool\n");
	fprintf(stderr, "--spike-log <path>         Record spike histogram data to <path>\n");
	fprintf(stderr, "--spike-log-interval-seconds []   Duration of histogram bins in seconds\n");

	fprintf(stderr, "--skip-test,    -s     Do not check if data is random enough.\n");
	fprintf(stderr, "--do-not-fork   -n     Do not fork.\n");
	fprintf(stderr, "--verbose,      -v     Be verbose.\n");
	fprintf(stderr, "--help,         -h     This help.\n");
	fprintf(stderr, "\n");
}

void daemonise(void)
{
	if (become_daemon() == -1)
		error_exit("cannot fork into the background");

	if (write_pidfile(PID_FILE) == -1)
		error_exit("Couldn't open PID file \"%s\" for writing: %m.", PID_FILE);
}

void gracefully_exit(int signum)
{
	if (got_mlockall) {
		if (munlockall() == -1)
			perror("munlockall");
	}
	unlink(PID_FILE);
	dolog(LOG_INFO, "audio-entropyd stopping due to signal %d", signum);
	exit(0);
}

void logging_handler(int signum)
{
	if (signum == SIGUSR1) 
	{
		loggingstate = 1; 
		dolog(LOG_WARNING, "Currently in flush state: entropy data is not random enough");
	}

	if (signum == SIGUSR2) 
		loggingstate = 0; 
}
