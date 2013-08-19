//
//  MRDPViewController.h
//  FreeRDP
//
//  Created by Richard Markiewicz on 2013-07-23.
//
//

#import <Cocoa/Cocoa.h>
#import "MRDPView.h"
#import "MRDPViewControllerDelegate.h"
#import "mfreerdp.h"

@interface MRDPViewController : NSViewController
{
    NSObject<MRDPViewControllerDelegate> *delegate;
    
    @public
	rdpContext* context;
	MRDPView* mrdpView;
}

@property(nonatomic, assign) NSObject<MRDPViewControllerDelegate> *delegate;
@property (assign) rdpContext *context;

- (BOOL)configure:(NSArray *)arguments;
- (void)start;
- (void)stop;

@end
