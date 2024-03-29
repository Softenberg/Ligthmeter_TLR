struct CameraExposureBare
{
    int aperture;
    int shutter;
    int iso;
};

struct CameraExposure
{
    CameraExposureBare prev;
    int aperture;
    int shutter;
    int iso;
};

enum CameraMode
{
    Aperture,
    Shutter,
    ISO,
};

struct CameraModeState
{
    CameraMode current;
    CameraMode prev;
};
