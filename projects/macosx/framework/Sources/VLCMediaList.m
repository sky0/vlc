/*****************************************************************************
 * VLCMediaList.m: VLCKit.framework VLCMediaList implementation
 *****************************************************************************
 * Copyright (C) 2007 Pierre d'Herbemont
 * Copyright (C) 2007 the VideoLAN team
 * $Id$
 *
 * Authors: Pierre d'Herbemont <pdherbemont # videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#import "VLCMediaList.h"
#import "VLCLibrary.h"
#import "VLCEventManager.h"
#import "VLCLibVLCBridging.h"
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc/vlc.h>
#include <vlc/libvlc.h>

/* Notification Messages */
NSString * VLCMediaListItemAdded        = @"VLCMediaListItemAdded";
NSString * VLCMediaListItemDeleted      = @"VLCMediaListItemDeleted";

// TODO: Documentation
@interface VLCMediaList (Private)
/* Initializers */
- (void)initInternalMediaList;

/* Libvlc event bridges */
- (void)mediaListItemAdded:(NSArray *)args;
- (void)mediaListItemRemoved:(NSNumber *)index;
@end

/* libvlc event callback */
static void HandleMediaListItemAdded(const libvlc_event_t * event, void * user_data)
{
    NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];
    id self = user_data;
    [[VLCEventManager sharedManager] callOnMainThreadObject:self 
                                                 withMethod:@selector(mediaListItemAdded:) 
                                       withArgumentAsObject:[NSArray arrayWithObject:[NSDictionary dictionaryWithObjectsAndKeys:
                                                          [VLCMedia mediaWithLibVLCMediaDescriptor:event->u.media_list_item_added.item], @"media",
                                                          [NSNumber numberWithInt:event->u.media_list_item_added.index], @"index",
                                                          nil]]];
    [pool release];
}

static void HandleMediaListItemDeleted( const libvlc_event_t * event, void * user_data)
{
    NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];
    id self = user_data;
    [[VLCEventManager sharedManager] callOnMainThreadObject:self 
                                                 withMethod:@selector(mediaListItemRemoved:) 
                                       withArgumentAsObject:[NSNumber numberWithInt:event->u.media_list_item_deleted.index]];
    [pool release];
}

@implementation VLCMediaList
- (id)init
{
    if (self = [super init])
    {
        // Create a new libvlc media list instance
        libvlc_exception_t p_e;
        libvlc_exception_init( &p_e );
        p_mlist = libvlc_media_list_new( [VLCLibrary sharedInstance], &p_e );
        catch_exception( &p_e );
        
        // Initialize internals to defaults
        cachedMedia = [[NSMutableArray alloc] init];
        delegate = flatAspect = hierarchicalAspect = hierarchicalNodeAspect = nil;
        [self initInternalMediaList];
    }
    return self;
}

- (void)release
{
    @synchronized(self)
    {
        if([self retainCount] <= 1)
        {
            /* We must make sure we won't receive new event after an upcoming dealloc
             * We also may receive a -retain in some event callback that may occcur
             * Before libvlc_event_detach. So this can't happen in dealloc */
            libvlc_event_manager_t * p_em = libvlc_media_list_event_manager(p_mlist, NULL);
            libvlc_event_detach(p_em, libvlc_MediaListItemDeleted, HandleMediaListItemDeleted, self, NULL);
            libvlc_event_detach(p_em, libvlc_MediaListItemAdded,   HandleMediaListItemAdded,   self, NULL);
        }
        [super release];
    }
}

- (void)dealloc
{
    // Release allocated memory
    delegate = nil;
    
    libvlc_media_list_release( p_mlist );
    [cachedMedia release];
    [flatAspect release];
    [hierarchicalAspect release];
    [hierarchicalNodeAspect release];
    [super dealloc];
}

- (NSString *)description
{
    NSMutableString * content = [NSMutableString string];
    int i;
    for( i = 0; i < [self count]; i++)
    {
        [content appendFormat:@"%@\n", [self mediaAtIndex: i]];
    }
    return [NSString stringWithFormat:@"<%@ %p> {\n%@}", [self className], self, content];
}

- (void)lock
{
    libvlc_media_list_lock( p_mlist );
}

- (void)unlock
{
    libvlc_media_list_unlock( p_mlist );
}

- (int)addMedia:(VLCMedia *)media
{
    int index = [self count];
    [self insertMedia:media atIndex:index];
    return index;
}

- (void)insertMedia:(VLCMedia *)media atIndex: (int)index
{
    [media retain];

    // Add it to the libvlc's medialist
    libvlc_exception_t p_e;
    libvlc_exception_init( &p_e );
    libvlc_media_list_insert_media( p_mlist, [media libVLCMediaDescriptor], index, &p_e );
    catch_exception( &p_e );
}

- (void)removeMediaAtIndex:(int)index
{
    [[self mediaAtIndex:index] release];

    // Remove it from the libvlc's medialist
    libvlc_exception_t p_e;
    libvlc_exception_init( &p_e );
    libvlc_media_list_remove_index( p_mlist, index, &p_e );
    catch_exception( &p_e );
}

- (VLCMedia *)mediaAtIndex:(int)index
{
    return [cachedMedia objectAtIndex:index];
}

- (int)indexOfMedia:(VLCMedia *)media
{
    libvlc_exception_t p_e;
    libvlc_exception_init( &p_e );
    int result = libvlc_media_list_index_of_item( p_mlist, [media libVLCMediaDescriptor], &p_e );
    catch_exception( &p_e );
    
    return result;
}

/* KVC Compliance: For the @"media" key */
- (int)countOfMedia
{
    return [self count];
}

- (id)objectInMediaAtIndex:(int)i
{
    return [self mediaAtIndex:i];
}

- (int)count
{
    return [cachedMedia count];
}

@synthesize delegate;

- (BOOL)isReadOnly
{
    libvlc_exception_t p_e;
    libvlc_exception_init( &p_e );
    BOOL isReadOnly = libvlc_media_list_is_readonly( p_mlist );
    catch_exception( &p_e );
    
    return isReadOnly;
}

/* Media list aspect */
- (VLCMediaListAspect *)hierarchicalAspect
{
    if( hierarchicalAspect )
        return hierarchicalAspect;
    
    libvlc_media_list_view_t * p_mlv = libvlc_media_list_hierarchical_view( p_mlist, NULL );
    hierarchicalAspect = [[VLCMediaListAspect mediaListAspectWithLibVLCMediaListView:p_mlv andMediaList:self] retain];
    libvlc_media_list_view_release( p_mlv );
    return hierarchicalAspect;
}

- (VLCMediaListAspect *)hierarchicalNodeAspect
{
    if( hierarchicalNodeAspect )
        return hierarchicalNodeAspect;
    
    libvlc_media_list_view_t * p_mlv = libvlc_media_list_hierarchical_node_view( p_mlist, NULL );
    hierarchicalNodeAspect = [[VLCMediaListAspect mediaListAspectWithLibVLCMediaListView:p_mlv andMediaList:self] retain];
    libvlc_media_list_view_release( p_mlv );
    return hierarchicalNodeAspect;
}

- (VLCMediaListAspect *)flatAspect
{
    if( flatAspect )
        return flatAspect;
    
    libvlc_media_list_view_t * p_mlv = libvlc_media_list_flat_view( p_mlist, NULL );
    flatAspect = [[VLCMediaListAspect mediaListAspectWithLibVLCMediaListView:p_mlv andMediaList:self] retain];
    libvlc_media_list_view_release( p_mlv );
    return flatAspect;
}

@end

@implementation VLCMediaList (LibVLCBridging)
+ (id)mediaListWithLibVLCMediaList:(void *)p_new_mlist;
{
    return [[[VLCMediaList alloc] initWithLibVLCMediaList:p_new_mlist] autorelease];
}

- (id)initWithLibVLCMediaList:(void *)p_new_mlist;
{
    if( self = [super init] )
    {
        p_mlist = p_new_mlist;
        libvlc_media_list_retain( p_mlist );
        libvlc_media_list_lock( p_mlist );
        cachedMedia = [[NSMutableArray alloc] initWithCapacity:libvlc_media_list_count( p_mlist, NULL )];

        int i, count = libvlc_media_list_count( p_mlist, NULL );
        for( i = 0; i < count; i++ )
        {
            libvlc_media_t * p_md = libvlc_media_list_item_at_index( p_mlist, i, NULL );
            [cachedMedia addObject:[VLCMedia mediaWithLibVLCMediaDescriptor:p_md]];
            libvlc_media_release(p_md);
        }
        [self initInternalMediaList];
        libvlc_media_list_unlock(p_mlist);
    }
    return self;
}

- (void *)libVLCMediaList
{
    return p_mlist;
}
@end

@implementation VLCMediaList (Private)
- (void)initInternalMediaList
{
    // Add event callbacks
    libvlc_exception_t p_e;
    libvlc_exception_init( &p_e );

    libvlc_event_manager_t * p_em = libvlc_media_list_event_manager( p_mlist, &p_e );
    libvlc_event_attach( p_em, libvlc_MediaListItemAdded,   HandleMediaListItemAdded,   self, &p_e );
    libvlc_event_attach( p_em, libvlc_MediaListItemDeleted, HandleMediaListItemDeleted, self, &p_e );
    
    catch_exception( &p_e );
}

- (void)mediaListItemAdded:(NSArray *)arrayOfArgs
{
    /* We hope to receive index in a nide range, that could change one day */
    int start = [[[arrayOfArgs objectAtIndex: 0] objectForKey:@"index"] intValue];
    int end = [[[arrayOfArgs objectAtIndex: [arrayOfArgs count]-1] objectForKey:@"index"] intValue];
    NSRange range = NSMakeRange(start, end-start);

    [self willChange:NSKeyValueChangeInsertion valuesAtIndexes:[NSIndexSet indexSetWithIndexesInRange:range] forKey:@"media"];
    for( NSDictionary * args in arrayOfArgs )
    {
        int index = [[args objectForKey:@"index"] intValue];
        VLCMedia * media = [args objectForKey:@"media"];
        /* Sanity check */
        if( index && index > [cachedMedia count] )
            index = [cachedMedia count];
        [cachedMedia insertObject:media atIndex:index];
    }
    [self didChange:NSKeyValueChangeInsertion valuesAtIndexes:[NSIndexSet indexSetWithIndexesInRange:range] forKey:@"media"];

    // Post the notification
//    [[NSNotificationCenter defaultCenter] postNotificationName:VLCMediaListItemAdded
//                                                        object:self
//                                                      userInfo:args];

    // Let the delegate know that the item was added
  //  if (delegate && [delegate respondsToSelector:@selector(mediaList:mediaAdded:atIndex:)])
    //    [delegate mediaList:self mediaAdded:media atIndex:index];
}

- (void)mediaListItemRemoved:(NSNumber *)index
{
    [self willChange:NSKeyValueChangeInsertion valuesAtIndexes:[NSIndexSet indexSetWithIndex:[index intValue]] forKey:@"media"];
    [cachedMedia removeObjectAtIndex:[index intValue]];
    [self didChange:NSKeyValueChangeInsertion valuesAtIndexes:[NSIndexSet indexSetWithIndex:[index intValue]] forKey:@"media"];

    // Post the notification
    [[NSNotificationCenter defaultCenter] postNotificationName:VLCMediaListItemDeleted 
                                                        object:self
                                                      userInfo:[NSDictionary dictionaryWithObjectsAndKeys:
                                                          index, @"index",
                                                          nil]];

    // Let the delegate know that the item is being removed
    if (delegate && [delegate respondsToSelector:@selector(mediaList:mediaRemovedAtIndex:)])
        [delegate mediaList:self mediaRemovedAtIndex:[index intValue]];
}
@end
