//
//  MRDPIPCClient.h
//  FreeRDP
//
//  Created by Richard Markiewicz on 2014-11-15.
//
//

#import <Foundation/Foundation.h>

// TODO! Fix references...
#import "../MRDPViewController.h"
#import "../MRDPClientDelegate.h"

@interface MRDPIPCClient : NSObject<MRDPClientDelegate>
{
    NSConnection *clientConnection;
    id serverProxy;
    rdpContext* context;
    MRDPClient* mrdpClient;
    int is_connected;
    bool is_stopped;
    NSRect* frame;
}

@property (assign) int is_connected;
@property (assign) NSRect* frame;

- (id)initWithServer:(NSString *)registeredName;
- (void)configure;

@end
