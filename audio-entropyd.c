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

#include <alsa/asoundlib.h>
#include <linux/soundcard.h>
#include <asm/types.h>
#include <linux/random.h>
#include <errno.h>

#include "proc.h"
#include "val.h"
#include "RNGTEST.h"
#include "error.h"

#define RANDOM_DEVICE				"/dev/random"
#define DEFAULT_SAMPLE_RATE			11025
#define PID_FILE				"/var/run/audio-entropyd.pid"
#define DEFAULT_CLICK_READ			(1 * DEFAULT_SAMPLE_RATE)
#define DEFAULT_POOLSIZE_FN                     "/proc/sys/kernel/random/poolsize"
#define	RNGTEST_PENALTY				(20000 / 8) /* how many bytes to skip when the rng-test fails */

void dolog(int level, char *format, ...);

extern int loggingstate;
int treshold = 0;
char skip_test = 0;
int error_state = 0;
char dofork = 1;
char *file = NULL;

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
		{"skip-test",	0, NULL, 's' },
		{"file",	1, NULL, 'f' },
		{"verbose",	0, NULL, 'v' },
		{"help",	0, NULL, 'h' },
		{NULL,		0, NULL, 0   }
	};

	/* Process commandline options */
	while(1)
	{
		c = getopt_long (argc, argv, "f:nsr:d:N:vh", long_options, NULL);
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

	dolog(LOG_NOTICE, "audio-entropyd starting up");

	if (mlockall(MCL_FUTURE | MCL_CURRENT) == -1)
		error_exit("mlockall failed");

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
	format = SND_PCM_FORMAT_S16_BE;
	err = snd_pcm_hw_params_set_format(chandle, ct_params, format);
	if (err < 0)
	{
		format = SND_PCM_FORMAT_S16_LE;
		err = snd_pcm_hw_params_set_format(chandle, ct_params, format);
	}
	if (err < 0)
		error_exit("Sample format (SND_PCM_FORMAT_S16_BE and _LE) not available for %s: %s", id, snd_strerror(err));

	/* Set stereo */
	err = snd_pcm_hw_params_set_channels(chandle, ct_params, 2);
	if (err < 0)
		error_exit("Channels count (%i) not available for %s: %s", 2, id, snd_strerror(err));

	/* Apply settings to sound device */
	err = snd_pcm_hw_params(chandle, ct_params);
	if (err < 0)
		error_exit("Could not apply settings to sound device!");

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

	/* first get some data so that we can immediately submit something when the
	 * kernel entropy-buffer gets below some limit
	 */
	get_random_data(sample_rate, DEFAULT_CLICK_READ, DEFAULT_SAMPLE_RATE, &n_output_bytes, &output_buffer);

	/* Main read loop */
	for(;;)
	{	
		int added = 0, before, loop, after;
		fd_set write_fd;
		FD_ZERO(&write_fd);
		FD_SET(random_fd, &write_fd);

		if (!file)
		{
			for(;;) 
			{ 
				int rc = select(random_fd+1, NULL, &write_fd, NULL, NULL); /* wait for krng */ 
				if (rc >= 0) break; 
				if (errno != EINTR) 
					error_exit("Select error: %m"); 
			}
		}

		/* find out how many bits to add */
		if (ioctl(random_fd, RNDGETENTCNT, &before) == -1)
			error_exit("Couldn't query entropy-level from kernel");

		dolog(LOG_DEBUG, "woke up due to low entropy state (%d bits left)", before);

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

			/* Get number of bits in KRNG after credit */
			if (ioctl(random_fd, RNDGETENTCNT, &after) == -1)
				error_exit("Coundn't query entropy-level from kernel: %m");

			if (verbose > 1 && after < max_bits)
				dolog(LOG_DEBUG, "minimum level not reached: %d", after);

			free(output_buffer);
			output_buffer = NULL;
			get_random_data(sample_rate, DEFAULT_CLICK_READ, DEFAULT_SAMPLE_RATE, &n_output_bytes, &output_buffer);
		}

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

void usage(void)
{
	fprintf(stderr, "Usage: audio-entropyd [options]\n\n");
	fprintf(stderr, "Collect entropy from a soundcard and feed it into the kernel random pool.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "--device,       -d []  Specify sound device to use. (Default %s)\n", cdevice);
	fprintf(stderr, "--sample-rate,  -N []  Audio sampling rate. (default %i)\n", DEFAULT_SAMPLE_RATE);
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
	if (munlockall() == -1)
		error_exit("problem unlocking pages");
	unlink(PID_FILE);
	dolog(LOG_NOTICE, "audio-entropyd stopping due to signal %d", signum);
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
