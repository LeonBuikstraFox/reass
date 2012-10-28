/*
 * Copyright 2011 Hylke Vellinga
 */


#include "pcap_reader.h"
#include "packet_listener.h"
#include "tcp_reassembler.h"
#include "udp_reassembler.h"
#include "config.h"
#include "shared/misc.h"

namespace
{
struct pcap_close_guard_t
{
	pcap_close_guard_t(pcap_t *&pcap) : d_pcap(pcap) {}
	~pcap_close_guard_t() { if (d_pcap) { pcap_close(d_pcap); d_pcap = nullptr; } }
	pcap_t *&d_pcap;
};

}; // end of nameless namespace

pcap_reader_t::pcap_reader_t(packet_listener_t *listener) :
	free_list_container_t<packet_t>(0),
	d_pcap(NULL), d_listener(listener),
#ifdef PRINT_STATS
	d_packetnr(0),
#endif
	d_tcp_reassembler(NULL), d_udp_reassembler(NULL)
{
	enable_tcp_reassembly(true);
	enable_udp_reassembly(true);
}

pcap_reader_t::~pcap_reader_t()
{
	flush();

	if (d_pcap) close_live_capture();

#ifdef PRINT_STATS
	printf("saw %ld packets\n", d_packetnr);
#if !defined(NO_REUSE) and defined(DEBUG)
	printf("max %d packet_t's in use\n", objectcount());
#endif
#endif // PRINT_STATS

	delete d_tcp_reassembler; d_tcp_reassembler = NULL;
	delete d_udp_reassembler; d_udp_reassembler = NULL;
}

void pcap_reader_t::read_file(const std::string &fname, const std::string &bpf)
{
	if (d_pcap)
		throw format_exception("Cannot read pcap while already busy");

	pcap_close_guard_t closeguard(d_pcap);

	open_file(fname, bpf);
	read_packets();
}

void pcap_reader_t::open_file(const std::string &fname, const std::string &bpf)
{
	if (d_pcap)
		throw format_exception("Cannot open pcap while already busy");

	char errbuf[PCAP_ERRBUF_SIZE];
	d_pcap = pcap_open_offline(fname.c_str(), errbuf);
	if (!d_pcap)
		throw format_exception("Could not open pcap '%s', %s", fname.c_str(), errbuf);

	set_bpf(bpf);

	d_linktype = pcap_datalink(d_pcap);

	d_listener->begin_capture(fname, linktype(), snaplen());
}

void pcap_reader_t::close_file()
{
	if (!d_pcap)
		throw format_exception("Cannot close pcap without opened pcap");

	pcap_close(d_pcap);
	d_pcap = nullptr;
}

void pcap_reader_t::open_live_capture(const std::string &device, bool promiscuous, const std::string &bpf)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	d_pcap = pcap_open_live(device.c_str(), 65536, promiscuous, 1000, errbuf);
	if (d_pcap == NULL)
		throw format_exception("Could not capture '%s', %s", device.c_str(), errbuf);

	set_bpf(bpf);

	d_linktype = pcap_datalink(d_pcap);

	d_listener->begin_capture(device, linktype(), snaplen());
}

void pcap_reader_t::set_bpf(const std::string &bpf)
{
	if (bpf.empty())
		return;

	// we don't specify the netmask, so filters for ipv4 broadcasts will fail
	if (pcap_compile(d_pcap, &d_bpf, bpf.c_str(), true, PCAP_NETMASK_UNKNOWN) < 0)
		throw format_exception("Could not compile bpf filter '%s', %s", bpf.c_str(), pcap_geterr(d_pcap));
	if (pcap_setfilter(d_pcap, &d_bpf) < 0)
		throw format_exception("Could not activate bpf filter '%s', %s", bpf.c_str(), pcap_geterr(d_pcap));
}


void pcap_reader_t::close_live_capture()
{
	pcap_close(d_pcap);
	d_pcap = nullptr;
}

void pcap_reader_t::set_listener(packet_listener_t *listener)
{
	if (d_tcp_reassembler)
		d_tcp_reassembler->set_listener(listener);
	if (d_udp_reassembler)
		d_udp_reassembler->set_listener(listener);
	d_listener = listener;
}

void pcap_reader_t::flush()
{
	if (d_tcp_reassembler)
		d_tcp_reassembler->flush();

	if (d_udp_reassembler)
		d_udp_reassembler->flush();
}

void pcap_reader_t::enable_tcp_reassembly(bool en)
{
	if (en && !d_tcp_reassembler)
		d_tcp_reassembler = new tcp_reassembler_t(d_listener);
	else if (!en && d_tcp_reassembler)
	{
		delete d_tcp_reassembler;
		d_tcp_reassembler = NULL;
	}
}

void pcap_reader_t::enable_udp_reassembly(bool en)
{
	if (en && !d_udp_reassembler)
		d_udp_reassembler = new udp_reassembler_t(d_listener);
	else if (!en && d_udp_reassembler)
	{
		delete d_udp_reassembler;
		d_udp_reassembler = NULL;
	}
}

// called whenever libpcap has a packet
void pcap_reader_t::handle_packet(const struct pcap_pkthdr *hdr, const u_char *data)
{
#ifdef PRINT_STATS
	++d_packetnr;
#endif

	packet_t *packet = claim(); // get space from free list (or new if free list was empty)

	bool must_copy = true; // packet is using libpcap memory
	try
	{
		packet->init(d_linktype, hdr, data, &must_copy); // parse layers

		if (d_tcp_reassembler)
			d_tcp_reassembler->set_now(packet->ts().tv_sec);
		if (d_udp_reassembler)
			d_udp_reassembler->set_now(packet->ts().tv_sec);

		// reassemble tcp if top-layer is tcp, or tcp+data
		layer_t *top = packet->layer(-1);
		layer_t *second = packet->layer(-2);
		if (!top || !second)
			d_listener->accept(packet); // less than two layers -> get rid of it
		else if (d_tcp_reassembler && (
					top->type() == layer_tcp ||
					(top->type() == layer_data && second->type() == layer_tcp)))
			d_tcp_reassembler->process(packet);
		else if (d_udp_reassembler && (
					top->type() == layer_udp ||
					(top->type() == layer_data && second->type() == layer_udp)))
			d_udp_reassembler->process(packet);
		else // don't know. just pass on packet
			d_listener->accept(packet);
	}
	catch(const unknown_layer_t &e)
	{
#ifdef UNKNOWN_LAYER_AS_ERROR
		d_listener->accept_error(packet, e.what());
#else
		packet->release();
#endif
	}
	catch(const std::exception &e)
	{
		d_listener->accept_error(packet, e.what());
	}
	if (must_copy)
		packet->copy_data();
}

#ifdef NO_MEMBER_CALLBACK
void extra_callback_hop(u_char *user, const struct pcap_pkthdr *hdr, const u_char *data)
{
	reinterpret_cast<pcap_reader_t *>(user)->handle_packet(hdr, data);
}
#endif

void pcap_reader_t::read_packets() // read one bufferful of packets
{
	assert(d_pcap);

#ifndef NO_MEMBER_CALLBACK
	// note: don't try this at home, kids
	pcap_handler handler = reinterpret_cast<pcap_handler>(&pcap_reader_t::handle_packet);
#else
	pcap_handler handler = &extra_callback_hop;
#endif
	int r = pcap_dispatch(d_pcap, -1, handler, (u_char *)this);
	if (r == -1)
		throw format_exception("Pcap reader failed, %s", pcap_geterr(d_pcap));
	//printf("got %d packets in read_packets\n", r);
}

