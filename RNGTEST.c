/* This RNG-tester was written by F.J.J. van Heusden (folkert@vanheusden.com).
 * It was implemented according to the FIPS1401-documentation which can be
 * found at http://csrc.nist.gov/publications/fips/index.html
 * The tests are implemented so one can do the test continuously. Beware,
 * though, that the tester will ALWAYS return 'not good' when there's not
 * data! (20.000 bits) I think there's no need to always also do the long-test.
 * I think it's only needed every 20.000-34=19966 bits (34=minimum for the
 * long-run test) which is about 2495,75 bytes (round down to be conservative:
 * 2495 bytes).
 *
 * usage:
 * 	before using this thing, do RNGTEST_init().
 *	it has no parameters and won't return anything at all
 *
 *	RNGTEST_short(): no parameters, returns 0 if not-so-random data, 1 if
 *	the data seems to be random
 *	RNGTEST_long(): see RNGTEST_short(). Note: it also invokes the short-
 * 	test
 *	RNGTEST(): calls RNGTEST_short(), and if enough new bits were added
 *	since the last _long()-test, it also calls the long one.
 *	RNGTEST_add(): adds 8 bits (1 byte) of data to internal bit-buffer
 *
 * Note: when an error occurs ( -> error = the data is not so random as one
 *      would expect), it'll take up to 20k bits before the tester says "ok"
 *      again. That is not a bug, it's expected behaviour.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>

int loggingstate = 0;

/* array with numberofbitssetto1 */
char RNGTEST_bit1cnt[256];

/* ringbuffer of 20000 bits */
unsigned char RNGTEST_rval[20000/8];
/* point to current, ehr, thing */
int RNGTEST_p;
/* number of bits in ringbuffer */
int RNGTEST_nbits;
/* number of new bits after a long-test */
int RNGTEST_nnewbits;

/* number of bits set to 1 (monobit test) */
int RNGTEST_n1;

/* for poker test */
int RNGTEST_pokerbuf[16];

void dolog(int level, char *format, ...)
{
        char buffer[4096];
        va_list ap;

	if (loggingstate)
	{
		va_start(ap, format);
		vsnprintf(buffer, sizeof(buffer), format, ap);
		va_end(ap);

		fprintf(stderr, "%s\n", buffer);
		syslog(level, "%s", buffer);
	}
}

void RNGTEST_init(void)
{
	int loop, bit;

	memset(RNGTEST_bit1cnt, 0x00, sizeof(RNGTEST_bit1cnt));
	memset(RNGTEST_rval, 0x00, sizeof(RNGTEST_rval));
	memset(RNGTEST_pokerbuf, 0x00, sizeof(RNGTEST_pokerbuf));

	RNGTEST_p = RNGTEST_nbits = RNGTEST_nnewbits = RNGTEST_n1 = 0;

	/* generate table with number of bits-set-to-1 for each number */
	for(loop=0; loop<256; loop++)
	{
		for(bit=1; bit<256; bit<<=1)
		{
			if (loop & bit)
				RNGTEST_bit1cnt[loop]++;
		}
	}
}

void RNGTEST_add(unsigned char newval)
{
	unsigned char old = RNGTEST_rval[RNGTEST_p];	/* get old value */
	RNGTEST_rval[RNGTEST_p] = newval;		/* remember new value */
	RNGTEST_p++;				/* go to next */
	if (RNGTEST_p == (20000/8)) RNGTEST_p=0;	/* ringbuffer */

	/* keep track of number of bits in ringbuffer */
	if (RNGTEST_nbits == 20000)
	{
		/* buffer full, forget old stuff */
		RNGTEST_n1 -= RNGTEST_bit1cnt[old];	/* monobit test */
		RNGTEST_pokerbuf[old & 15]--;	/* poker test */
		RNGTEST_pokerbuf[old >> 4]--;
	}
	else	/* another 8 bits added */
	{
		RNGTEST_nbits += 8;
	}

	/* keep track of # new bits since last longtest */
	if (RNGTEST_nnewbits < 20000) /* prevent overflowwraps */
	{
		RNGTEST_nnewbits += 8;
	}

	/* there must be about 50% of 1's in the bitstream
	 * (monobit test)
	 */
	RNGTEST_n1 += RNGTEST_bit1cnt[newval];		/* keep track of n1-counts */

	/* poker test */
	RNGTEST_pokerbuf[newval & 15]++;	/* do the 2 nibbles */
	RNGTEST_pokerbuf[newval >> 4]++;
}

char RNGTEST_shorttest(void)
{
	int loop;
	int total=0;
	double X;

	/* we can only say anything on this data when there had been
	 * enough data to evaluate
	 */
	if (RNGTEST_nbits != 20000)
	{
#ifdef _DEBUG
		fprintf(stderr, "Not enought data for test (%d bits left).\n", 20000-RNGTEST_nbits);
#endif
		return 0;
	}

	/* monobit test */
#if 0	/* 140-1 */
	if (RNGTEST_n1<=9654 || RNGTEST_n1 >= 10346)	/* passed if 9654 < n1 < 10346 */
#endif
		/* 140-2 */
		if (RNGTEST_n1<=9725 || RNGTEST_n1 >= 10275)	/* passwd if 9725 < n1 < 10275 */
		{
			dolog(LOG_CRIT, "Monobit test failed! [%d]", RNGTEST_n1);
			return -1;
		}

	/* poker test */
	/*  X = (16/5000) * (E[f(i)]^2, 0<=i<=15) - 5000
	 * -passed if 1.03 < X < 57.4 <-- 140-1
	 * +passwd if 2.16 < X < 46.17 <-- 140-2
	 */
	for(loop=0; loop<16; loop++)
	{
		total += (RNGTEST_pokerbuf[loop]*RNGTEST_pokerbuf[loop]);
	}
	X = (16.0/5000.0) * ((double)total) - 5001.0;
#if 0	/* 140-1 */
	if ((X<=1.03) || (X>=57.4))
#endif
		/* 140-2 */
		if ((X<=2.16) || (X>=46.17))
		{
			dolog(LOG_CRIT, "Poker test failed! [%f]", X);
			return -1;
		}

	/* well, as far as we could see here, all is fine */
	return 0;
}

#define RNGTEST_checkinterval(index, min, max)						\
	((runlencounts[(index)][0]<=(min) || runlencounts[(index)][0]>=(max) || \
	  runlencounts[(index)][1]<=(min) || runlencounts[(index)][1]>=(max))	\
	 ? 0 : 1)

/* warning; this one also invokes the short test(!) */
char RNGTEST_longtest(void)
{
	int byteindex;
	int lastbit=0;
	int runlength=0;
	int runlencounts[7][2];
	char nok=0;
	memset(runlencounts, 0x00, sizeof(runlencounts));

	/* first see if the shorttest fails. no need to do
	 * the long one if the short one is failing already
	 */
	if (RNGTEST_shorttest() == 0)
	{
		return -1;
	}

	/* go trough all 20.000 bits */
	for(byteindex=0; byteindex<(20000/8); byteindex++)
	{
		int bitindex;

		/* get a byte */
		unsigned char curbyte = RNGTEST_rval[byteindex];

		/* test all bits in this byte */
		for(bitindex=0; bitindex<8; bitindex++)
		{
			/* first bit? */
			if (byteindex==0 && bitindex==0)
			{
				lastbit = (curbyte & 128)?1:0;
				runlength = 1;
			}
			else	/* not the first bit, so evaluate */
			{
				int curbit = (curbyte & 128)?1:0;

				/* this bit is the same as the previous one */
				if (curbit == lastbit)
				{
					runlength++;

					/* test for long-run (34 or more bits
					 * with same value) */
#if 0 /* 140-1 */
					if (runlength >= 34)
#endif
						if (runlength >= 26)	/* 140-2 */
						{
							dolog(LOG_CRIT, "Long-run failed! [%d]", runlength);
							return -1;
						}
				}
				else
				{
					/* remember this bit */
					lastbit = curbit;

					/* keep track of run-lengths */
					if (runlength > 6){runlength=6;}
					(runlencounts[runlength][curbit])++;

					/* reset to runlength=1 */
					runlength = 1;
				}
			}

			/* go the next bit */
			curbyte <<= 1;
		}
	}
	/* take also care of the last run! */
	if (runlength)
	{
		/* keep track of run-lengths */
		if (runlength > 6){runlength=6;}
		runlencounts[runlength][lastbit]++;
	}

	/* now we evaluated all bits, reset new-bits-counter */
	RNGTEST_nnewbits = 0;

	/* now we have the frequencies of all runs */
	/* verify their frequency of occurence */
#if 0	/* 140-1 */
	nok |= !RNGTEST_checkinterval(1, 2267, 2733);
	nok |= !RNGTEST_checkinterval(2, 1079, 1421);
	nok |= !RNGTEST_checkinterval(3, 502, 748);
	nok |= !RNGTEST_checkinterval(4, 223, 402);
	nok |= !RNGTEST_checkinterval(5, 90, 223);
	nok |= !RNGTEST_checkinterval(6, 90, 223);
#endif
	/* 140-2 */
	nok |= !RNGTEST_checkinterval(1, 2343, 2657);
	nok |= !RNGTEST_checkinterval(2, 1135, 1365);
	nok |= !RNGTEST_checkinterval(3, 542, 708);
	nok |= !RNGTEST_checkinterval(4, 251, 373);
	nok |= !RNGTEST_checkinterval(5, 111, 201);
	nok |= !RNGTEST_checkinterval(6, 111, 201);
	if (nok)
	{
		dolog(LOG_CRIT, "Runs-test failed!");

		return -1;
	}

	/* this is a fine set of random values */
	return 0;
}

char RNGTEST(void)
{
	if (RNGTEST_nnewbits >= 2495)
	{
		return RNGTEST_longtest();
	}

	return RNGTEST_shorttest();
}
