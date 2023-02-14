#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>

void geodist_( double *Eplat, double *Eplon, double *Stlat, double *Stlon, double *Az, double *Baz, double *Dist ) {
    /*
        JHT: In actual fact, I use the first two arguments for "My Location",
            the second two for "His location"; West longitude is positive.

             Taken directly from:
             Thomas, P.D., 1970, Spheroidal geodesics, reference systems,
             & local geometry, U.S. Naval Oceanographi!Office SP-138,
             165 pp.
             assumes North Latitude and East Longitude are positive

             EpLat, EpLon = End point Lat/Long
             Stlat, Stlon = Start point lat/long
             Az, BAz = direct & reverse azimuith
             Dist = Dist (km); Deg = central angle, discarded
    */
    double BOA, F, P1R, P2R, L1R, L2R, DLR, T1R, T2R, TM;
    double DTM, STM, CTM, SDTM,CDTM, KL, KK, SDLMR, L;
    double CD, DL, SD, T, U, V, D, X, E, Y, A, FF64, TDLPM;
    double HAPBR, HAMBR, A1M2, A2M1;

    double AL = 6378206.4;              // Clarke 1866 ellipsoid
    double BL = 6356583.8;
    double D2R = 0.01745329251994;      // degrees to radians conversion factor
    double Pi2 = 6.28318530718;

    BOA = BL/AL;
    F = 1.0 - BOA;
    // Convert st/end pts to radians
    P1R = (*Eplat) * D2R;
    P2R = (*Stlat) * D2R;
    L1R = (*Eplon) * D2R;
    L2R = (*Stlon) * D2R;
    DLR = L2R - L1R;                 // DLR = Delta Long in Rads
    T1R = atan(BOA * tan(P1R));
    T2R = atan(BOA * tan(P2R));
    TM = (T1R + T2R) / 2.0;
    DTM = (T2R - T1R) / 2.0;
    STM = sin(TM);
    CTM = cos(TM);
    SDTM = sin(DTM);
    CDTM = cos(DTM);
    KL = STM * CDTM;
    KK = SDTM * CTM;
    SDLMR = sin(DLR/2.0);
    L = SDTM * SDTM + SDLMR * SDLMR * (CDTM * CDTM - STM * STM);
    CD = 1.0 - 2.0 * L;
    DL = acos(CD);
    SD = sin(DL);
    T = DL/SD;
    U = 2.0 * KL * KL / (1.0 - L);
    V = 2.0 * KK * KK / L;
    D = 4.0 * T * T;
    X = U + V;
    E = -2.0 * CD;
    Y = U - V;
    A = -D * E;
    FF64 = F * F / 64.0;
    (*Dist) = AL*SD*(T -(F/4.0)*(T*X-Y)+FF64*(X*(A+(T-(A+E)/2.0)*X)+Y*(-2.0*D+E*Y)+D*X*Y))/1000.0;
    TDLPM = tan((DLR+(-((E*(4.0-X)+2.0*Y)*((F/2.0)*T+FF64*(32.0*T+(A-20.0*T)*X-2.0*(D+2.0)*Y))/4.0)*tan(DLR)))/2.0);
    HAPBR = atan2(SDTM,(CTM*TDLPM));
    HAMBR = atan2(CDTM,(STM*TDLPM));
    A1M2 = Pi2 + HAMBR - HAPBR;
    A2M1 = Pi2 - HAMBR - HAPBR;

L1:
    if ((A1M2 >= 0.0) && (A1M2 < Pi2)) { goto L5; }
    if (A1M2 < Pi2) { goto L4; }
    A1M2 = A1M2 - Pi2;
    goto L1;
L4:
    A1M2 = A1M2 + Pi2;
    goto L1;

    // All of this gens the proper az, baz (forward and back azimuth)

L5:
    if ((A2M1 >= 0.0) && (A2M1 < Pi2)) { goto L9; }
    if (A2M1 < Pi2) { goto L8; }
    A2M1 = A2M1 - Pi2;
    goto L5;
L8:
    A2M1 = A2M1 + Pi2;
    goto L5;

L9:
    *Az = A1M2 / D2R;
    *Baz = A2M1 / D2R;

    // Fix the mirrored coords here.

    *Az = 360.0 - (*Az);
    *Baz = 360.0 - (*Baz);
}
