/*
Copyright (c) 2013, David C Horton

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#include <stdexcept>
#include <mutex>

#include <boost/functional/hash.hpp>

#include <sofia-sip/msg_addr.h>
#include <sofia-sip/su_addrinfo.h>
#include <sofia-sip/nta.h>
#include <sofia-sip/sip_util.h>

#include "sip-dialog.hpp"
#include "controller.hpp"


namespace {
    /* needed to be able to live in a boost unordered container */
    size_t hash_value( const drachtio::SipDialog& d) {
        std::size_t seed = 0;
        boost::hash_combine(seed, d.getCallId().c_str());
        boost::hash_combine(seed, d.getLocalEndpoint().m_strTag.c_str());
        boost::hash_combine(seed, d.getRemoteEndpoint().m_strTag.c_str());
        return seed;
    }

    void session_timer_handler( su_root_magic_t* magic, su_timer_t* timer, su_timer_arg_t* args) {
    	std::weak_ptr<drachtio::SipDialog> *p = reinterpret_cast< std::weak_ptr<drachtio::SipDialog> *>( args ) ;
    	std::shared_ptr<drachtio::SipDialog> pDialog = p->lock() ;
    	if( pDialog ) pDialog->doSessionTimerHandling() ;
    	else assert(0) ;
    }

  	std::mutex sd_mutex;
}

namespace drachtio {
	
	/* dialog generated by an incoming INVITE */
	SipDialog::SipDialog( nta_leg_t* leg, nta_incoming_t* irq, sip_t const *sip, msg_t* msg ) : m_type(we_are_uas), m_recentSipStatus(100), 
		m_startTime(time(NULL)), m_connectTime(0), m_endTime(0), m_releaseCause(no_release), m_refresher(no_refresher), m_timerSessionRefresh(NULL),m_ppSelf(NULL),
		m_nSessionExpiresSecs(0), m_nMinSE(90), m_tp(nta_incoming_transport(theOneAndOnlyController->getAgent(), irq, msg) ), 
    m_leg( leg ), m_timerG(NULL), m_durationTimerG(0), m_timerH(NULL), m_orqAck(nullptr), m_orq(nullptr), m_seq(0),
		m_bInviteDialog(sip->sip_request->rq_method == sip_method_invite), m_bAlerting(false), m_nSessionTimerDuration(0),
		m_timeArrive(std::chrono::steady_clock::now()), m_bAckBye(false), m_tmArrival(sip_now()), m_bDestroyAckOnClose(false)
	{
    const tp_name_t* tpn = tport_name( m_tp );

    this->setSourceAddress( nta_incoming_remote_host(irq) )  ;
    this->setSourcePort( ::atoi( nta_incoming_remote_port(irq) ) ) ;
    m_protocol = nta_incoming_protocol(irq) ;

    m_transportAddress = tpn->tpn_host ;
    m_transportPort = tpn->tpn_port ;

 
		/* get remaining values from the headers */
		if( sip->sip_call_id->i_id  ) m_strCallId = sip->sip_call_id->i_id ;
		const char*  rtag = nta_leg_get_rtag( leg )  ;
		if( rtag ) this->setRemoteTag( rtag ) ;
		assert(rtag); // should always have a from tag on incoming invite
	
		if( sip->sip_payload ) this->setRemoteSdp( sip->sip_payload->pl_data, sip->sip_payload->pl_len ) ;
		if( sip->sip_content_type ) {
			string hvalue ;
			parseGenericHeader( sip->sip_content_type->c_common, hvalue ) ;
			if( !hvalue.empty() ) this->setRemoteContentType( hvalue ) ;			
		}

		// UDP nat check: if no Record-Route and Contact != source address:port, then set a RouteUri to the source address:port
		// update: if there is a Record-Route and topmost Record-Route has nat=yes in the url param, do the same as above
		if (tport_is_dgram(m_tp)) {
			bool nat = false;
			if ( theOneAndOnlyController->isAggressiveNatEnabled() && sipMsgHasNatEqualsYes(sip, false, false)) {
				DR_LOG(log_info) << "SipDialog::SipDialog - (UAS) detected nat=yes in Contact or Record-Route, using  " << m_sourceAddress << ":" << m_sourcePort << " as route for requests within this dialog";
				nat = true;
			}
			else if (!theOneAndOnlyController->isNatDetectionDisabled() && sip->sip_contact && !sip->sip_record_route) {
				const url_t* url = sip->sip_contact->m_url;
				if (url && (0 != m_sourceAddress.compare(url->url_host) || (url->url_port && atoi(url->url_port) != m_sourcePort))) {
					DR_LOG(log_info) << "SipDialog::SipDialog - (UAS) detected client behind nat, using  " << m_sourceAddress << ":" << m_sourcePort << " as route for requests within this dialog";
					nat = true;
				}
			}

			if (nat) {
				url_t const * url = nta_incoming_url(irq);
				m_routeUri = url->url_scheme;
				m_routeUri.append(":");
				m_routeUri.append(m_sourceAddress); 
				m_routeUri.append(":");
				m_routeUri.append(boost::lexical_cast<string>(m_sourcePort));
			}
		}

    DR_LOG(log_debug) << "SipDialog::SipDialog - creating dialog for inbound INVITE sent from " << m_protocol << "/" << m_transportAddress << ":" << m_transportPort ;

	}

	/* dialog generated by an outgoing INVITE */
	SipDialog::SipDialog( const string& transactionId, nta_leg_t* leg, 
		nta_outgoing_t* orq, sip_t const *sip, msg_t *msg, const string& transport) : m_type(we_are_uac), m_recentSipStatus(0), 
		m_startTime(0), m_connectTime(0), m_endTime(0), m_releaseCause(no_release), m_refresher(no_refresher), m_timerSessionRefresh(NULL),m_ppSelf(NULL),
		m_nSessionExpiresSecs(0), m_nMinSE(90), m_tp(NULL), m_leg(leg), m_orqAck(nullptr), m_orq(orq), m_seq(0),
    m_timerG(NULL), m_durationTimerG(0), m_timerH(NULL), m_nSessionTimerDuration(0),
		m_bInviteDialog(sip->sip_request->rq_method == sip_method_invite), m_bAlerting(false),
		m_timeArrive(std::chrono::steady_clock::now()), m_bAckBye(false), m_tmArrival(sip_now()), m_bDestroyAckOnClose(false)
	{
		m_transactionId = transactionId ;

		if( sip->sip_call_id->i_id ) m_strCallId = sip->sip_call_id->i_id ;
		m_seq = nta_leg_get_seq(leg);

    m_tp = nta_outgoing_transport( orq );

    if( m_tp ) {
      const tp_name_t* tpn = tport_name( m_tp );

      m_transportAddress = tpn->tpn_host ;
      m_transportPort = tpn->tpn_port ;
      m_protocol = tpn->tpn_proto ;      
    }
    else {
      parseTransportDescription(transport, m_protocol, m_transportAddress, m_transportPort ) ;
    }

		const char *ltag = nta_leg_get_tag( leg ) ;
		if( ltag ) this->setLocalTag( ltag ) ;
		assert(ltag); // should always have a from tag on incoming invite

		if( sip->sip_payload ) this->setLocalSdp( sip->sip_payload->pl_data, sip->sip_payload->pl_len ) ;
		if( sip->sip_content_type ) {
			string hvalue ;
			parseGenericHeader( sip->sip_content_type->c_common, hvalue ) ;
			if( !hvalue.empty() ) this->setLocalContentType( hvalue ) ;			
		}

    su_sockaddr_t const *su = msg_addr(msg);
    char name[SU_ADDRSIZE] = "";

    su_inet_ntop(su->su_family, SU_ADDR(su), name, sizeof(name));
    unsigned int port = ntohs(su->su_port)  ;

    if( NULL != strstr( name, ":") ) {
      this->setSourceAddress( string("[") + name + string("]") ) ;  // ipv6
    }
    else {
      this->setSourceAddress( name ) ;
    }

    this->setSourcePort( port ) ;

		const url_t* urlRoute = nta_outgoing_route_uri(orq);
		if (urlRoute) {
			su_home_t* home = msg_home(msg);
			const char * route_uri = url_as_string(home, urlRoute);
			m_routeUri = route_uri;
			su_free(home, (void *) route_uri);
		}

		DR_LOG(log_debug) << "SipDialog::SipDialog - creating dialog for outbound INVITE sent from " << m_protocol << "/" << m_transportAddress << ":" << m_transportPort << " to " << name << ":" << std::dec << port ;

	}	
	SipDialog::~SipDialog() {
		DR_LOG(log_debug) << "SipDialog::~SipDialog - destroying sip dialog with call-id " << getCallId() ;
		if( NULL != m_timerSessionRefresh ) {
			cancelSessionTimer() ;
			assert( m_ppSelf ) ;
		}
		if( m_ppSelf ) {
			delete m_ppSelf ;
		}

		nta_leg_t *leg = nta_leg_by_call_id( theOneAndOnlyController->getAgent(), getCallId().c_str() );
		assert( leg ) ;
		if( leg ) {
			nta_leg_destroy( leg ) ;
		}
    if( NULL != m_tp ) {
      tport_unref( m_tp ) ;
    }

		/* stop timer D if it is running */
		if (we_are_uac == m_type && m_orq) {
			theOneAndOnlyController->getDialogController()->stopTimerD(m_orq);
		}
		/* N.B.: we only destroy the orq here for ACK for non-udp transports, since timer D handles for udp */
		if (m_orqAck && m_bDestroyAckOnClose) {
			DR_LOG(log_debug) << "SipDialog::~SipDialog - destroying orq from original (uac) ACK " << std::hex << (void *) m_orqAck ;
			nta_outgoing_destroy(m_orqAck);
		}
		if (m_orq) {
			nta_outgoing_destroy(m_orq);
		}
	}

	std::ostream& operator<<(std::ostream& os, const SipDialog& dlg) {
    sip_time_t alive = sip_now() - dlg.m_tmArrival;
    os << "dialogId:" << dlg.dialogId() << std::dec << 
      " alive:" << alive << "s" << std::hex << 
      " callid:" << dlg.getCallId() << 
      " role:" << (dlg.getRole() == SipDialog::we_are_uac ? "UAC" : "UAS") << 
      " leg:" << dlg.getNtaLeg() ;
    return os;
  }

  void SipDialog::checkTportState(void) {
    if (m_tp && tport_is_closed(m_tp)) {
      DR_LOG(log_debug) << "SipDialog::checkTportState: tport(" << std::hex << (void *) m_tp << ") has been closed, releasing";
      tport_unref(m_tp);
      m_tp = nullptr;
    }
  }
  void SipDialog::setTport(tport_t* tp) {
		if (m_tp) tport_unref(m_tp);
    tport_ref( tp ) ;
    m_tp = tp ;
    const tp_name_t* tpn = tport_name( m_tp );

    m_transportAddress = tpn->tpn_host ;
    m_transportPort = tpn->tpn_port ;
    m_protocol = tpn->tpn_proto ;      
  }
	tport_t* SipDialog::getTport(void) { 
		tport_t* tp = nullptr;

    checkTportState();
		if (m_tp) tp = m_tp ;
		else if (m_orqAck) {
			tp = nta_outgoing_transport( m_orqAck );
			if (tp) {
        if (tport_is_closed(tp)) {
          tport_unref(tp);
          tp = nullptr;
        }
        else {
          DR_LOG(log_debug) << "SipDialog::getTport: retrieving tport from delayed orq " << std::hex << (void *) m_orqAck << ": " << (void *) tp;
          m_tp = tp;
        }
        nta_outgoing_destroy(m_orqAck);
        m_orqAck = nullptr; 
			}
		}
		return tp;
	}

	void SipDialog::setSessionTimer( unsigned long nSecs, SessionRefresher_t whoIsResponsible ) {
		if (m_timerSessionRefresh) cancelSessionTimer();
		m_refresher = whoIsResponsible ;
		m_nSessionTimerDuration = nSecs * 1000  ;
		m_nSessionExpiresSecs = nSecs ;

		DR_LOG(log_info) << "SipDialog::setSessionTimer: " << getCallId() << " Session expires has been set to " << nSecs << " seconds and refresher is " << (areWeRefresher() ? "us" : "them")  ;

		/* if we are the refresher, then we want the timer to go off halfway through the interval */
		if( areWeRefresher() ) m_nSessionTimerDuration /= 2 ;
		m_timerSessionRefresh = su_timer_create( su_root_task(theOneAndOnlyController->getRoot()), m_nSessionTimerDuration ) ;

		m_ppSelf = new std::weak_ptr<SipDialog>( shared_from_this() ) ;
		su_timer_set(m_timerSessionRefresh, session_timer_handler, (su_timer_arg_t *) m_ppSelf );
	}
	void SipDialog::cancelSessionTimer() {
		assert( NULL != m_timerSessionRefresh ) ;
		if (m_timerSessionRefresh) su_timer_destroy( m_timerSessionRefresh ) ;
		m_timerSessionRefresh = NULL ;
		m_refresher = no_refresher ;
		m_nSessionExpiresSecs = 0 ;
	}
	void SipDialog::doSessionTimerHandling() {
		bool bWeAreRefresher = areWeRefresher()  ;
		
		if( bWeAreRefresher ) {
			//send a refreshing reINVITE, and notify the client
			DR_LOG(log_info) << "SipDialog::doSessionTimerHandling - sending refreshing re-INVITE with call-id " << getCallId()  ; 
			theOneAndOnlyController->getDialogController()->notifyRefreshDialog( shared_from_this() ) ;
		}
		else {
			//tear down the leg, and notify the client
			DR_LOG(log_info) << "SipDialog::doSessionTimerHandling - tearing down sip dialog with call-id " << getCallId() 
				<< " because remote peer did not refresh the session within the specified interval"  ; 
			theOneAndOnlyController->getDialogController()->notifyTerminateStaleDialog( shared_from_this() ) ;
		}

		assert( m_ppSelf ) ;
		delete m_ppSelf ; m_ppSelf = NULL ; m_timerSessionRefresh = NULL ; m_refresher = no_refresher ; m_nSessionExpiresSecs = 0 ;
	}

	void SD_Insert(StableDialogs_t& dialogs, std::shared_ptr<SipDialog>& dlg) {
    std::lock_guard<std::mutex> lock(sd_mutex) ;
		auto& idx = dialogs.get<DlgPtrTag>();
    auto res = idx.insert(dlg);
		if (!res.second) {
	    DR_LOG(log_error) << "SD_Insert failed to insert dialog " << *dlg;
		}
	}

	bool SD_FindByLeg(const StableDialogs_t& dialogs, nta_leg_t* leg, std::shared_ptr<SipDialog>& dlg) {
		std::lock_guard<std::mutex> lock(sd_mutex) ;
    auto &idx = dialogs.get<DlgLegTag>();
    auto it = idx.find(leg);
    if (it == idx.end()) return false;
    dlg = *it;
    return true;
	}
	bool SD_FindByDialogId(const StableDialogs_t& dialogs, const std::string& dialogId, std::shared_ptr<SipDialog>& dlg) {
		std::lock_guard<std::mutex> lock(sd_mutex) ;
    auto &idx = dialogs.get<DialogIdTag>();
    auto it = idx.find(dialogId);
    if (it == idx.end()) return false;
    dlg = *it;
    return true;
	}
  void SD_Clear(StableDialogs_t& dialogs, std::shared_ptr<SipDialog>& dlg) {
    std::lock_guard<std::mutex> lock(sd_mutex) ;

    auto &idx = dialogs.get<DlgPtrTag>();
    idx.erase(dlg);
	}

  void SD_Clear(StableDialogs_t& dialogs, const std::string& dialogId) {
    std::lock_guard<std::mutex> lock(sd_mutex) ;

    auto &idx = dialogs.get<DialogIdTag>();
    idx.erase(dialogId);
	}

  void SD_Clear(StableDialogs_t& dialogs, nta_leg_t* leg) {
    std::lock_guard<std::mutex> lock(sd_mutex) ;

    auto &idx = dialogs.get<DlgLegTag>();
    idx.erase(leg);
	}

  size_t SD_Size(const StableDialogs_t& dialogs) {
    std::lock_guard<std::mutex> lock(sd_mutex) ;
    auto &idx = dialogs.get<DlgPtrTag>();
    return idx.size();
	}

  size_t SD_Size(const StableDialogs_t& dialogs, size_t& nUac, size_t& nUas) {
    std::lock_guard<std::mutex> lock(sd_mutex) ;
    auto &idx = dialogs.get<DlgPtrTag>();
    size_t total = idx.size();
		auto &idxRole = dialogs.get<DlgRoleTag>();
		nUac = idxRole.count(SipDialog::we_are_uac);
		nUas = idxRole.count(SipDialog::we_are_uas);
		return total;
	}

  void SD_Log(const StableDialogs_t& dialogs, bool full) {
		size_t count, nUac, nUas;
		count = SD_Size(dialogs, nUac, nUas);
    DR_LOG(log_debug) << "StableDialogs total size:                                                " << count;
    DR_LOG(log_debug) << "StableDialogs uac:                                                       " << nUac;
    DR_LOG(log_debug) << "StableDialogs uas:                                                       " << nUas;
    if (full && count) {
      std::lock_guard<std::mutex> lock(sd_mutex) ;
      auto &idx = dialogs.get<DlgTimeTag>();
      for (auto it = idx.begin(); it != idx.end(); ++it) {
				std::shared_ptr<SipDialog> p = *it;
        DR_LOG(log_debug) << *p;
      }
    }

	}

} 
