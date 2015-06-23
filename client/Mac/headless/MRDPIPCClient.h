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
#import "../ServerCertificate.h"
#import "../X509Certificate.h"
#import "../ServerCredential.h"

@interface MRDPIPCClient : NSObject<MRDPClientDelegate>
{
    NSConnection *clientConnection;
    id serverProxy;
    rdpContext* context;
    MRDPClient* mrdpClient;
    bool is_stopped;
    NSRect frame;
}

@property (assign) NSRect frame;

- (id)initWithServer:(NSString *)registeredName;
- (void)configure;
- (void)initLoggingWithFilter:(NSString *)filter filePath:(NSString *)filePath fileName:(NSString *)fileName;

@end
