#include <string.h>
#include <math.h>

#define min(x, y)       ((x)<(y)?(x):(y))

double calc_nbits_in_data(unsigned char *data, int nbytestoprocess)
{
        int cnts[256], loop;
        double ent=0.0;
        memset(cnts, 0x00, sizeof(cnts));

        for(loop=0; loop<nbytestoprocess; loop++)
        {
                cnts[data[loop]]++;
        }

        for(loop=0; loop<256;loop++)
        {
                double prob = (double)cnts[loop] / (double)nbytestoprocess;

                if (prob > 0.0)
                {
                        ent += prob * (log(1.0/prob)/log(2.0));
                }
        }

        ent *= (double)nbytestoprocess;

        if (ent < 0.0) ent=0.0;

        ent = min((double)(nbytestoprocess*8), ent);

        return ent;
}
