
#include <TargetConditionals.h>
#if TARGET_OS_IOS == 1
  #import <UIKit/UIKit.h>
#else
  #import <Cocoa/Cocoa.h>
#endif

#define IPLUG_AUVIEWCONTROLLER IPlugAUViewController_vNeuralRig
#define IPLUG_AUAUDIOUNIT IPlugAUAudioUnit_vNeuralRig
#import <NeuralRigAU/IPlugAUAudioUnit.h>
#import <NeuralRigAU/IPlugAUViewController.h>

//! Project version number for NeuralRigAU.
FOUNDATION_EXPORT double NeuralRigAUVersionNumber;

//! Project version string for NeuralRigAU.
FOUNDATION_EXPORT const unsigned char NeuralRigAUVersionString[];

@class IPlugAUViewController_vNeuralRig;
