
/*
 * NetJack - Packet Handling functions
 *
 * used by the driver and the jacknet_client
 *
 * Copyright (C) 2006 Torben Hohn <torbenh@gmx.de>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: net_driver.c,v 1.16 2006/03/20 19:41:37 torbenh Exp $
 *
 */


#include <math.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>

#include <jack/types.h>
#include <jack/engine.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <samplerate.h>

#include "net_driver.h"
#include "netjack_packet.h"

int fraggo = 0;

packet_cache *global_packcache;

void
packet_header_hton(jacknet_packet_header *pkthdr)
{
    pkthdr->channels = htonl(pkthdr->channels);
    pkthdr->period_size = htonl(pkthdr->period_size);
    pkthdr->sample_rate = htonl(pkthdr->sample_rate);
    pkthdr->sync_state = htonl(pkthdr->sync_state);
    pkthdr->transport_frame = htonl(pkthdr->transport_frame);
    pkthdr->transport_state = htonl(pkthdr->transport_state);
    pkthdr->framecnt = htonl(pkthdr->framecnt);
    pkthdr->latency = htonl(pkthdr->latency);
    pkthdr->reply_port = htonl(pkthdr->reply_port);
    pkthdr->mtu = htonl(pkthdr->mtu);
    pkthdr->fragment_nr = htonl(pkthdr->fragment_nr);
}

void
packet_header_ntoh(jacknet_packet_header *pkthdr)
{
    pkthdr->channels = ntohl(pkthdr->channels);
    pkthdr->period_size = ntohl(pkthdr->period_size);
    pkthdr->sample_rate = ntohl(pkthdr->sample_rate);
    pkthdr->sync_state = ntohl(pkthdr->sync_state);
    pkthdr->transport_frame = ntohl(pkthdr->transport_frame);
    pkthdr->transport_state = ntohl(pkthdr->transport_state);
    pkthdr->framecnt = ntohl(pkthdr->framecnt);
    pkthdr->latency = ntohl(pkthdr->latency);
    pkthdr->reply_port = ntohl(pkthdr->reply_port);
    pkthdr->mtu = ntohl(pkthdr->mtu);
    pkthdr->fragment_nr = ntohl(pkthdr->fragment_nr);
}

int get_sample_size(int bitdepth)
{
    if (bitdepth == 8)
        return sizeof(int8_t);
    if (bitdepth == 16)
        return sizeof(int16_t);
    return sizeof(int32_t);
}

// fragment management functions.

packet_cache *packet_cache_new(int num_packets, int pkt_size, int mtu)
{
    int fragment_payload_size = mtu - sizeof(jacknet_packet_header);
    int fragment_number = (pkt_size - sizeof(jacknet_packet_header) - 1) / fragment_payload_size + 1;
    int i;

    packet_cache *pcache = malloc(sizeof(packet_cache));
    if (pcache == NULL) {
        printf("could not allocate packet cache (1)\n");
        return NULL;
    }

    pcache->size = num_packets;
    pcache->packets = malloc(sizeof(cache_packet) * num_packets);
    if (pcache->packets == NULL) {
        printf("could not allocate packet cache (2)\n");
        return NULL;
    }

    for (i = 0; i < num_packets; i++) {
        pcache->packets[i].valid = 0;
        pcache->packets[i].num_fragments = fragment_number;
        pcache->packets[i].packet_size = pkt_size;
        pcache->packets[i].mtu = mtu;
        pcache->packets[i].framecnt = 0;
        pcache->packets[i].fragment_array = malloc(sizeof(char) * fragment_number);
        pcache->packets[i].packet_buf = malloc(pkt_size);

        if ((pcache->packets[i].fragment_array == NULL) || (pcache->packets[i].packet_buf == NULL)) {
            printf("could not allocate packet cache (3)\n");
            return NULL;
        }
    }

    return pcache;
}

void packet_cache_free(packet_cache *pcache)
{
    int i;

    for (i = 0; i < pcache->size; i++) {
        free(pcache->packets[i].fragment_array);
        free(pcache->packets[i].packet_buf);
    }

    free(pcache->packets);
    free(pcache);
}

cache_packet *packet_cache_get_packet(packet_cache *pcache, jack_nframes_t framecnt)
{
    int i;
    cache_packet *retval;

    for (i = 0; i < pcache->size; i++) {
        if (pcache->packets[i].valid && (pcache->packets[i].framecnt == framecnt))
            return &(pcache->packets[i]);
    }

    // The Packet is not in the packet cache.
    // find a free packet.

    retval = packet_cache_get_free_packet(pcache);
    if (retval != NULL) {
        cache_packet_set_framecnt(retval, framecnt);
        return retval;
    }

    // No Free Packet available
    // Get The Oldest packet and reset it.

    retval = packet_cache_get_oldest_packet(pcache);
    cache_packet_reset(retval);
    cache_packet_set_framecnt(retval, framecnt);

    return retval;
}

cache_packet *packet_cache_get_oldest_packet(packet_cache *pcache)
{
    jack_nframes_t minimal_frame = 0;
    cache_packet *retval = &(pcache->packets[0]);
    int i;

    for (i = 0; i < pcache->size; i++) {
        if (pcache->packets[i].valid && (pcache->packets[i].framecnt < minimal_frame)) {
            minimal_frame = pcache->packets[i].framecnt;
            retval = &(pcache->packets[i]);
        }
    }

    return retval;
}


cache_packet *packet_cache_get_free_packet(packet_cache *pcache)
{
    int i;

    for (i = 0; i < pcache->size; i++) {
        if (pcache->packets[i].valid == 0)
            return &(pcache->packets[i]);
    }

    return NULL;
}

void cache_packet_reset(cache_packet *pack)
{
    int i;
    pack->valid = 0;

    // XXX: i dont think this is necessary here...
    //      fragement array is cleared in _set_framecnt()

    for (i = 0; i < pack->num_fragments; i++)
        pack->fragment_array[i] = 0;
}

void cache_packet_set_framecnt(cache_packet *pack, jack_nframes_t framecnt)
{
    int i;

    pack->framecnt = framecnt;

    for (i = 0; i < pack->num_fragments; i++)
        pack->fragment_array[i] = 0;

    pack->valid = 1;
}

void cache_packet_add_fragment(cache_packet *pack, char *packet_buf, int rcv_len)
{
    jacknet_packet_header *pkthdr = (jacknet_packet_header *) packet_buf;
    int fragment_payload_size = pack->mtu - sizeof(jacknet_packet_header);
    char *packet_bufX = pack->packet_buf + sizeof(jacknet_packet_header);
    char *dataX = packet_buf + sizeof(jacknet_packet_header);

    jack_nframes_t fragment_nr = ntohl(pkthdr->fragment_nr);
    jack_nframes_t framecnt    = ntohl(pkthdr->framecnt);

    if (framecnt != pack->framecnt) {
        printf("errror. framecnts dont match\n");
        return;
    }

    if (fragment_nr == 0) {
        memcpy(pack->packet_buf, packet_buf, pack->mtu);
        pack->fragment_array[0] = 1;

        return;
    }

    if ((fragment_nr < pack->num_fragments) && (fragment_nr > 0)) {
        if ((fragment_nr * fragment_payload_size + rcv_len - sizeof(jacknet_packet_header)) <= (pack->packet_size - sizeof(jacknet_packet_header))) {
            memcpy(packet_bufX + fragment_nr * fragment_payload_size, dataX, rcv_len - sizeof(jacknet_packet_header));
            pack->fragment_array[fragment_nr] = 1;
        } else {
            printf("too long packet received...");
        }
    }
}

int cache_packet_is_complete(cache_packet *pack)
{
    int i;
    for (i = 0; i < pack->num_fragments; i++)
        if (pack->fragment_array[i] == 0)
            return FALSE;

    return TRUE;
}

// fragmented packet IO

int netjack_recvfrom(int sockfd, char *packet_buf, int pkt_size, int flags, struct sockaddr *addr, socklen_t *addr_size, int mtu)
{
    if (pkt_size <= mtu) {
        return recvfrom(sockfd, packet_buf, pkt_size, flags, addr, addr_size);
    } else {

        char *rx_packet = alloca(mtu);
        jacknet_packet_header *pkthdr = (jacknet_packet_header *)rx_packet;
        int rcv_len;
        jack_nframes_t framecnt;

        cache_packet *cpack;

rx_again:
        rcv_len = recvfrom(sockfd, rx_packet, mtu, 0, addr, addr_size);
        if (rcv_len < 0)
            return rcv_len;

        framecnt = ntohl(pkthdr->framecnt);

        cpack = packet_cache_get_packet(global_packcache, framecnt);
        cache_packet_add_fragment(cpack, rx_packet, rcv_len);
        if (cache_packet_is_complete(cpack)) {
            memcpy(packet_buf, cpack->packet_buf, pkt_size);
            cache_packet_reset(cpack);
            return pkt_size;
        }

        goto rx_again;
    }
}

int netjack_recv(int sockfd, char *packet_buf, int pkt_size, int flags, int mtu)
{

    if (pkt_size <= mtu) {
        return recv(sockfd, packet_buf, pkt_size, flags);
    } else {

        char *rx_packet = alloca(mtu);
        jacknet_packet_header *pkthdr = (jacknet_packet_header *)rx_packet;
        int rcv_len;
        jack_nframes_t framecnt;

        cache_packet *cpack;

rx_again:
        rcv_len = recv(sockfd, rx_packet, mtu, flags);
        if (rcv_len < 0)
            return rcv_len;

        framecnt = ntohl(pkthdr->framecnt);

        cpack = packet_cache_get_packet(global_packcache, framecnt);
        cache_packet_add_fragment(cpack, rx_packet, rcv_len);
        if (cache_packet_is_complete(cpack)) {
            memcpy(packet_buf, cpack->packet_buf, pkt_size);
            cache_packet_reset(cpack);
            return pkt_size;
        }

        goto rx_again;
    }
}
#if 0
int netjack_recvfrom(int sockfd, char *packet_buf, int pkt_size, int flags, struct sockaddr *addr, socklen_t *addr_size, int mtu)
{
    char *rx_packet;
    char *dataX;
    int i;

    int fragment_payload_size;

    char *fragment_array;
    int   fragment_number;
    int   rx_frag_count, framenum;

    jacknet_packet_header *pkthdr;

    // Now loop and send all
    char *packet_bufX;

    // wait for fragment_nr == 0
    int rcv_len;

    rx_packet = alloca(mtu);
    dataX = rx_packet + sizeof(jacknet_packet_header);

    fragment_payload_size = mtu - sizeof(jacknet_packet_header);

    pkthdr = (jacknet_packet_header *)rx_packet;

    packet_bufX = packet_buf + sizeof(jacknet_packet_header);

    fragment_number = (pkt_size - sizeof(jacknet_packet_header) - 1) / fragment_payload_size + 1;
    fragment_array = alloca(fragment_number);
    for (i = 0; i < fragment_number; i++)
        fragment_array[i] = 0;

    if (pkt_size <= mtu) {
        return recvfrom(sockfd, packet_buf, pkt_size, flags, addr, addr_size);
    } else {
rx_again:
        rcv_len = recvfrom(sockfd, rx_packet, mtu, 0, addr, addr_size);
        if (rcv_len < 0)
            return rcv_len;

        if (rcv_len >= sizeof(jacknet_packet_header)) {
            //printf("got fragmentooooo_nr = %d recv_len = %d\n",  ntohl(pkthdr->fragment_nr), rcv_len);
            if ((ntohl(pkthdr->fragment_nr)) != 0)
                goto rx_again;
        } else {
            goto rx_again;
        }

        // ok... we have read a fragement 0;
        // copy the data into the packet buffer...
        memcpy(packet_buf, rx_packet, mtu);

        rx_frag_count = 1;
        framenum = ntohl(pkthdr->framecnt);

        fragment_array[0] = 1;

        while (rx_frag_count < fragment_number) {


            rcv_len = recvfrom(sockfd, rx_packet, mtu, 0, addr, addr_size);
            if (rcv_len < 0)
                return -1;

            if (ntohl(pkthdr->framecnt) < framenum) {
                //printf("Out of Order Framecnt: i abort on this packet\n");
                printf("Old Fragment !!! (got: %d, exp: %d)\n", ntohl(pkthdr->framecnt),  framenum);
                continue;
            }

            if (ntohl(pkthdr->framecnt) > framenum) {
                printf("Newer Fragment !!! (got: %d, exp: %d) Switching to new Packet.\n", ntohl(pkthdr->framecnt),  framenum);
                // Copy the new Packetheader up Front
                memcpy(packet_buf, rx_packet, sizeof(jacknet_packet_header));

                rx_frag_count = 0;
                framenum = ntohl(pkthdr->framecnt);
            }
#if 0
            if (ntohl(pkthdr->framecnt) > framenum) {
                printf("Newer Fragment !!! (got: %d, exp: %d) Dropping it\n", ntohl(pkthdr->framecnt),  framenum);
                continue;
            }
#endif

            // copy the payload into the packet buffer...
            if ((ntohl(pkthdr->fragment_nr) < fragment_number) && (ntohl(pkthdr->fragment_nr) >= 0)) {
                if ((ntohl(pkthdr->fragment_nr) * fragment_payload_size + rcv_len - sizeof(jacknet_packet_header)) <= (pkt_size - sizeof(jacknet_packet_header))) {
                    memcpy(packet_bufX + (ntohl(pkthdr->fragment_nr) * fragment_payload_size), dataX, rcv_len - sizeof(jacknet_packet_header));
                    rx_frag_count++;
                } else {
                    printf("too long packet received...");
                }
            }
		}
	}
    return pkt_size;
}
#endif

#if 0
int netjack_recv(int sockfd, char *packet_buf, int pkt_size, int flags, int mtu)
{
	char *rx_packet;
    char *dataX;
    int i;

    int fragment_payload_size;

    char *fragment_array;
    int   fragment_number;
    int   rx_frag_count, framenum;

    jacknet_packet_header *pkthdr;

    // Now loop and send all
    char *packet_bufX;

    // wait for fragment_nr == 0
    int rcv_len;

    rx_packet = alloca(mtu);
    dataX = rx_packet + sizeof(jacknet_packet_header);

    fragment_payload_size = mtu - sizeof(jacknet_packet_header);

    pkthdr = (jacknet_packet_header *)rx_packet;

    packet_bufX = packet_buf + sizeof(jacknet_packet_header);

    fragment_number = (pkt_size - sizeof(jacknet_packet_header) - 1) / fragment_payload_size + 1;
    fragment_array = alloca(fragment_number);
    for (i = 0; i < fragment_number; i++)
        fragment_array[i] = 0;

    if (pkt_size <= mtu) {
        return recv(sockfd, packet_buf, pkt_size, flags);
    } else {
rx_again:
        rcv_len = recv(sockfd, rx_packet, mtu, flags);
        if (rcv_len < 0)
            return rcv_len;

        if (rcv_len >= sizeof(jacknet_packet_header)) {
            if ((ntohl(pkthdr->fragment_nr)) != 0)
                goto rx_again;
        } else {
            goto rx_again;
        }

        // ok... we have read a fragement 0;
        // copy the data into the packet buffer...
        memcpy(packet_buf, rx_packet, mtu);

        rx_frag_count = 1;
        framenum = ntohl(pkthdr->framecnt);

        fragment_array[0] = 1;

        while (rx_frag_count < fragment_number) {

            rcv_len = recv(sockfd, rx_packet, mtu, flags);
            if (rcv_len < 0)
                return -1;
///////////////////
            if (ntohl(pkthdr->framecnt) < framenum) {
                //printf("Out of Order Framecnt: i abort on this packet\n");
                printf("Old Fragment !!! (got: %d, exp: %d)\n", ntohl(pkthdr->framecnt),  framenum);
                continue;
            }

#if 0
            if (ntohl(pkthdr->framecnt) > framenum) {
                printf("Newer Fragment !!! (got: %d, exp: %d) Switching to new Packet.\n", ntohl(pkthdr->framecnt),  framenum);
                // Copy the new Packetheader up Front
                memcpy(packet_buf, rx_packet, sizeof(jacknet_packet_header));

                rx_frag_count = 0;
                framenum = ntohl(pkthdr->framecnt);
            }
#endif
            if (ntohl(pkthdr->framecnt) > framenum) {
                printf("Newer Fragment !!! (got: %d, exp: %d) Dropping it\n", ntohl(pkthdr->framecnt),  framenum);
                continue;
            }

/////////////////////////////////
//
            // copy the payload into the packet buffer...
            if ((ntohl(pkthdr->fragment_nr) < fragment_number) && (ntohl(pkthdr->fragment_nr) >= 0)) {
                if ((ntohl(pkthdr->fragment_nr) * fragment_payload_size + rcv_len - sizeof(jacknet_packet_header)) <= (pkt_size - sizeof(jacknet_packet_header))) {
                    memcpy(packet_bufX + (ntohl(pkthdr->fragment_nr) * fragment_payload_size), dataX, rcv_len - sizeof(jacknet_packet_header));
                    rx_frag_count++;
                } else {
                    printf("too long packet received...");
                }
            }
        }
    }
    return pkt_size;
}
#endif

void netjack_sendto(int sockfd, char *packet_buf, int pkt_size, int flags, struct sockaddr *addr, int addr_size, int mtu)
{
    int frag_cnt = 0;
    char *tx_packet, *dataX;
    jacknet_packet_header *pkthdr;

    tx_packet = alloca(mtu + 10);
    dataX = tx_packet + sizeof(jacknet_packet_header);
    pkthdr = (jacknet_packet_header *)tx_packet;

    int fragment_payload_size = mtu - sizeof(jacknet_packet_header);

    if (pkt_size <= mtu) {
        sendto(sockfd, packet_buf, pkt_size, flags, addr, addr_size);
    } else {

        // Copy the packet header to the tx pack first.
        memcpy(tx_packet, packet_buf, sizeof(jacknet_packet_header));

        // Now loop and send all
        char *packet_bufX = packet_buf + sizeof(jacknet_packet_header);

        while (packet_bufX < (packet_buf + pkt_size - fragment_payload_size)) {
            pkthdr->fragment_nr = htonl(frag_cnt++);
            memcpy(dataX, packet_bufX, fragment_payload_size);
            sendto(sockfd, tx_packet, mtu, flags, addr, addr_size);
            packet_bufX += fragment_payload_size;
        }

        int last_payload_size = packet_buf + pkt_size - packet_bufX;
        memcpy(dataX, packet_bufX, last_payload_size);
        pkthdr->fragment_nr = htonl(frag_cnt);
        //printf("last fragment_count = %d, payload_size = %d\n", fragment_count, last_payload_size);

        // sendto(last_pack_size);
        sendto(sockfd, tx_packet, last_payload_size + sizeof(jacknet_packet_header), flags, addr, addr_size);
    }
}

// render functions for float
void render_payload_to_jack_ports_float( void *packet_payload, jack_nframes_t net_period_down, JSList *capture_ports, JSList *capture_srcs, jack_nframes_t nframes)
{
    channel_t chn = 0;
    JSList *node = capture_ports;
    JSList *src_node = capture_srcs;

    uint32_t *packet_bufX = (uint32_t *)packet_payload;

    while (node != NULL) {
        int i;
        int_float_t val;
        SRC_DATA src;

        jack_port_t *port = (jack_port_t *) node->data;
        jack_default_audio_sample_t* buf = jack_port_get_buffer (port, nframes);

        if (net_period_down != nframes) {
            SRC_STATE *src_state = src_node->data;
            for (i = 0; i < net_period_down; i++) {
                packet_bufX[i] = ntohl(packet_bufX[i]);
            }

            src.data_in = (float *)packet_bufX;
            src.input_frames = net_period_down;

            src.data_out = buf;
            src.output_frames = nframes;

            src.src_ratio = (float) nframes / (float) net_period_down;
            src.end_of_input = 0;

            src_set_ratio(src_state, src.src_ratio);
            src_process(src_state, &src);
            src_node = jack_slist_next (src_node);
        } else {
            for (i = 0; i < net_period_down; i++) {
                val.i = packet_bufX[i];
                val.i = ntohl(val.i);
                buf[i] = val.f;
            }
        }

        packet_bufX = (packet_bufX + net_period_down);
        node = jack_slist_next (node);
        chn++;
    }
}

void render_jack_ports_to_payload_float(JSList *playback_ports, JSList *playback_srcs, jack_nframes_t nframes, void *packet_payload, jack_nframes_t net_period_up)
{
    channel_t chn = 0;
    JSList *node = playback_ports;
    JSList *src_node = playback_srcs;

    uint32_t *packet_bufX = (uint32_t *)packet_payload;

    while (node != NULL) {
        SRC_DATA src;
        int i;
        int_float_t val;
        jack_port_t *port = (jack_port_t *) node->data;
        jack_default_audio_sample_t* buf = jack_port_get_buffer (port, nframes);

        if (net_period_up != nframes) {
            SRC_STATE *src_state = src_node->data;
            src.data_in = buf;
            src.input_frames = nframes;

            src.data_out = (float *) packet_bufX;
            src.output_frames = net_period_up;

            src.src_ratio = (float) net_period_up / (float) nframes;
            src.end_of_input = 0;

            src_set_ratio(src_state, src.src_ratio);
            src_process(src_state, &src);

            for (i = 0; i < net_period_up; i++) {
                packet_bufX[i] = htonl(packet_bufX[i]);
            }
            src_node = jack_slist_next (src_node);
        } else {
            for (i = 0; i < net_period_up; i++) {
                val.f = buf[i];
                val.i = htonl(val.i);
                packet_bufX[i] = val.i;
            }
        }

        packet_bufX = (packet_bufX + net_period_up);
        node = jack_slist_next (node);
        chn++;
    }
}

// render functions for 16bit
void render_payload_to_jack_ports_16bit(void *packet_payload, jack_nframes_t net_period_down, JSList *capture_ports, JSList *capture_srcs, jack_nframes_t nframes)
{
    channel_t chn = 0;
    JSList *node = capture_ports;
    JSList *src_node = capture_srcs;

    uint16_t *packet_bufX = (uint16_t *)packet_payload;

    while (node != NULL) {
        int i;
        //uint32_t val;
        SRC_DATA src;

        jack_port_t *port = (jack_port_t *) node->data;
        jack_default_audio_sample_t* buf = jack_port_get_buffer (port, nframes);

        float *floatbuf = alloca(sizeof(float) * net_period_down);

        if (net_period_down != nframes) {
            SRC_STATE *src_state = src_node->data;
            for (i = 0; i < net_period_down; i++) {
                floatbuf[i] = ((float) ntohs(packet_bufX[i])) / 32767.0 - 1.0;
            }

            src.data_in = floatbuf;
            src.input_frames = net_period_down;

            src.data_out = buf;
            src.output_frames = nframes;

            src.src_ratio = (float) nframes / (float) net_period_down;
            src.end_of_input = 0;

            src_set_ratio(src_state, src.src_ratio);
            src_process(src_state, &src);
            src_node = jack_slist_next (src_node);
        } else {
            for (i = 0; i < net_period_down; i++) {
                buf[i] = ((float) ntohs(packet_bufX[i])) / 32768.0 - 1.0;
            }
        }

        packet_bufX = (packet_bufX + net_period_down);
        node = jack_slist_next (node);
        chn++;
    }
}

void render_jack_ports_to_payload_16bit(JSList *playback_ports, JSList *playback_srcs, jack_nframes_t nframes, void *packet_payload, jack_nframes_t net_period_up)
{
    channel_t chn = 0;
    JSList *node = playback_ports;
    JSList *src_node = playback_srcs;

    uint16_t *packet_bufX = (uint16_t *)packet_payload;

    while (node != NULL) {
        SRC_DATA src;
        int i;
        jack_port_t *port = (jack_port_t *) node->data;
        jack_default_audio_sample_t* buf = jack_port_get_buffer (port, nframes);

        if (net_period_up != nframes) {
            SRC_STATE *src_state = src_node->data;

            float *floatbuf = alloca(sizeof(float) * net_period_up);

            src.data_in = buf;
            src.input_frames = nframes;

            src.data_out = floatbuf;
            src.output_frames = net_period_up;

            src.src_ratio = (float) net_period_up / (float) nframes;
            src.end_of_input = 0;

            src_set_ratio(src_state, src.src_ratio);
            src_process(src_state, &src);

            for (i = 0; i < net_period_up; i++) {
                packet_bufX[i] = htons((floatbuf[i] + 1.0) * 32767.0);
            }
            src_node = jack_slist_next (src_node);
        } else {
            for (i = 0; i < net_period_up; i++) {
                packet_bufX[i] = htons((buf[i] + 1.0) * 32767.0);
            }
        }

        packet_bufX = (packet_bufX + net_period_up);
        node = jack_slist_next (node);
        chn++;
    }
}

// render functions for 8bit
void render_payload_to_jack_ports_8bit(void *packet_payload, jack_nframes_t net_period_down, JSList *capture_ports, JSList *capture_srcs, jack_nframes_t nframes)
{
    channel_t chn = 0;
    JSList *node = capture_ports;
    JSList *src_node = capture_srcs;

    int8_t *packet_bufX = (int8_t *)packet_payload;

    while (node != NULL) {
        int i;
        //uint32_t val;
        SRC_DATA src;

        jack_port_t *port = (jack_port_t *) node->data;
        jack_default_audio_sample_t* buf = jack_port_get_buffer (port, nframes);

        float *floatbuf = alloca(sizeof(float) * net_period_down);

        if (net_period_down != nframes) {
            SRC_STATE *src_state = src_node->data;
            for (i = 0; i < net_period_down; i++) {
                floatbuf[i] = ((float) packet_bufX[i]) / 127.0;
            }

            src.data_in = floatbuf;
            src.input_frames = net_period_down;

            src.data_out = buf;
            src.output_frames = nframes;

            src.src_ratio = (float) nframes / (float) net_period_down;
            src.end_of_input = 0;

            src_set_ratio(src_state, src.src_ratio);
            src_process(src_state, &src);
            src_node = jack_slist_next (src_node);
        } else {
            for (i = 0; i < net_period_down; i++) {
                buf[i] = ((float) packet_bufX[i]) / 127.0;
            }
        }

        packet_bufX = (packet_bufX + net_period_down);
        node = jack_slist_next (node);
        chn++;
    }
}

void render_jack_ports_to_payload_8bit(JSList *playback_ports, JSList *playback_srcs, jack_nframes_t nframes, void *packet_payload, jack_nframes_t net_period_up)
{
    channel_t chn = 0;
    JSList *node = playback_ports;
    JSList *src_node = playback_srcs;

    int8_t *packet_bufX = (int8_t *)packet_payload;

    while (node != NULL) {
        SRC_DATA src;
        int i;
        jack_port_t *port = (jack_port_t *) node->data;
        jack_default_audio_sample_t* buf = jack_port_get_buffer (port, nframes);

        if (net_period_up != nframes) {
            SRC_STATE *src_state = src_node->data;

            float *floatbuf = alloca(sizeof(float) * net_period_up);

            src.data_in = buf;
            src.input_frames = nframes;

            src.data_out = floatbuf;
            src.output_frames = net_period_up;

            src.src_ratio = (float) net_period_up / (float) nframes;
            src.end_of_input = 0;

            src_set_ratio(src_state, src.src_ratio);
            src_process(src_state, &src);

            for (i = 0; i < net_period_up; i++) {
                packet_bufX[i] = floatbuf[i] * 127.0;
            }
            src_node = jack_slist_next (src_node);
        } else {
            for (i = 0; i < net_period_up; i++) {
                packet_bufX[i] = buf[i] * 127.0;
            }
        }

        packet_bufX = (packet_bufX + net_period_up);
        node = jack_slist_next (node);
        chn++;
    }
}

// wrapper functions with bitdepth argument...
void render_payload_to_jack_ports(int bitdepth, void *packet_payload, jack_nframes_t net_period_down, JSList *capture_ports, JSList *capture_srcs, jack_nframes_t nframes)
{
    if (bitdepth == 8)
        render_payload_to_jack_ports_8bit(packet_payload, net_period_down, capture_ports, capture_srcs, nframes);
    else if (bitdepth == 16)
        render_payload_to_jack_ports_16bit(packet_payload, net_period_down, capture_ports, capture_srcs, nframes);
    else
        render_payload_to_jack_ports_float(packet_payload, net_period_down, capture_ports, capture_srcs, nframes);
}

void render_jack_ports_to_payload (int bitdepth, JSList *playback_ports, JSList *playback_srcs, jack_nframes_t nframes, void *packet_payload, jack_nframes_t net_period_up)
{
    if (bitdepth == 8)
        render_jack_ports_to_payload_8bit(playback_ports, playback_srcs, nframes, packet_payload, net_period_up);
    else if (bitdepth == 16)
        render_jack_ports_to_payload_16bit(playback_ports, playback_srcs, nframes, packet_payload, net_period_up);
    else
        render_jack_ports_to_payload_float(playback_ports, playback_srcs, nframes, packet_payload, net_period_up);
}
