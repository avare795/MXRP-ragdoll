/*
 * mxb_api.h - structures de l'API de plugins de MX Bikes (PiBoSo)
 *
 * Extrait de l'exemple officiel de PiBoSo, tel que repris dans le projet
 * open source mxbmrp3 (https://github.com/thomas4f/mxbmrp3, licence MIT).
 * Seules les structures utilisées par ce plugin sont reprises ici.
 *
 * Versions de l'interface attendues par MX Bikes beta 21 :
 *   GetModID() = "mxbikes", GetModDataVersion() = 8, GetInterfaceVersion() = 9
 */
#ifndef MXB_API_H
#define MXB_API_H

#define MXB_MOD_ID            "mxbikes"
#define MXB_MOD_DATA_VERSION  8
#define MXB_INTERFACE_VERSION 9

#ifdef _WIN32
#define MXB_EXPORT __declspec(dllexport)
#else
#define MXB_EXPORT
#endif

typedef struct {
    char m_szRiderName[100];
    char m_szBikeID[100];
    char m_szBikeName[100];
    int m_iNumberOfGears;
    int m_iMaxRPM;
    int m_iLimiter;
    int m_iShiftRPM;
    float m_fEngineOptTemperature;
    float m_afEngineTemperatureAlarm[2];
    float m_fMaxFuel;
    float m_afSuspMaxTravel[2];
    float m_fSteerLock;
    char m_szCategory[100];
    char m_szTrackID[100];
    char m_szTrackName[100];
    float m_fTrackLength;
    int m_iType;                         /* 1 = testing; 2 = race; 4 = straight rhythm */
    char m_szServerName[64];
    int m_iServerType;
    char m_szGUID[100];
} SPluginsBikeEvent_t;

typedef struct {
    int m_iRPM;
    float m_fEngineTemperature;
    float m_fWaterTemperature;
    int m_iGear;
    float m_fFuel;
    float m_fSpeedometer;                /* m/s */
    float m_fPosX, m_fPosY, m_fPosZ;     /* point de référence du châssis (pas le CG) */
    float m_fVelocityX, m_fVelocityY, m_fVelocityZ; /* vitesse du CG, monde, m/s */
    float m_fAccelerationX, m_fAccelerationY, m_fAccelerationZ; /* en G, repère châssis */
    float m_aafRot[3][3];                /* matrice de rotation du châssis */
    float m_fYaw, m_fPitch, m_fRoll;     /* degrés */
    float m_fYawVelocity, m_fPitchVelocity, m_fRollVelocity; /* degrés/s */
    float m_afSuspLength[2];
    float m_afSuspVelocity[2];
    int m_iCrashed;                      /* 1 = pilote détaché de la moto */
    float m_fSteer;
    float m_fThrottle;
    float m_fFrontBrake;
    float m_fRearBrake;
    float m_fClutch;
    float m_afWheelSpeed[2];
    int m_aiWheelMaterial[2];            /* 0 = roue en l'air */
    float m_afBrakePressure[2];
    float m_fSteerTorque;
} SPluginsBikeData_t;

typedef struct {
    float m_aafPos[4][2];                /* 0,0 = haut gauche ; 1,1 = bas droite. sens anti-horaire */
    int m_iSprite;                       /* index base 1 ; 0 = couleur unie m_ulColor */
    unsigned long m_ulColor;             /* ABGR */
} SPluginQuad_t;

typedef struct {
    char m_szString[100];
    float m_afPos[2];
    int m_iFont;                         /* index base 1 */
    float m_fSize;
    int m_iJustify;                      /* 0 = gauche ; 1 = centre ; 2 = droite */
    unsigned long m_ulColor;             /* ABGR */
} SPluginString_t;

#endif /* MXB_API_H */
