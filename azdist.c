#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>

void grid2deg_( char *grid, double *dlong, double *dlat );
void geodist_( double *dlat1, double *dlong1, double *dlat2, double *dlong2, double *Az, double *Baz, double *Dkm );


void azdist_( char *myGrid, char *hisGrid, int* nAz,
               int* nDmiles, int* nDkm) {
    // nAz, nDmiles, and nDkm are the only parameters that need to be filled in
    double dlong1,dlat1,dlong2,dlat2;
    double Az,Baz,Dkm;
    double temp,temp2,fract;

    if (strcmp(myGrid,hisGrid) == 0) {
        *nAz = 0;
        *nDmiles = 0;
        *nDkm = 0;
        return;
    }

    if (myGrid[4] == ' ') { myGrid[4] = 'm'; }
    if (myGrid[5] == ' ') { myGrid[5] = 'm'; }
    if (hisGrid[4] == ' ') { hisGrid[4] = 'm'; }
    if (hisGrid[5] == ' ') { hisGrid[5] = 'm'; }

    grid2deg_( myGrid, &dlong1, &dlat1);
    grid2deg_( hisGrid, &dlong2, &dlat2);

    //  All six values are doubles.  The first four are not changed so I don't know if they
    //      can be passed by reference.
    geodist_( &dlat1, &dlong1, &dlat2, &dlong2, &Az, &Baz, &Dkm );

    *nDkm = (int)Dkm;
    fract = modf(Dkm,&temp);
    if (fract >= 0.5) {
        *nDkm = (*nDkm)+1;
    }

    temp2 = Dkm/1.609344;
    fract = modf(temp2,&temp);
    *nDmiles = (int)temp2;
    if (fract >= 0.5) {
        *nDmiles = (*nDmiles)+1;
    }

    *nAz = (int)Az;
    fract = modf(Az,&temp);
    if (fract >= 0.5) {
        *nAz = (*nAz)+1;
    }
}
