//
// Copyright (C) 2010 Alberto Cortés Martín
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include <omnetpp.h>
#include "SCTPAp.h"
#include "SCTPCommand_m.h"

SCTPAp::SCTPAp(bool enable,
               double periodSecs,
               int burst,
               double giveUpSecs,
               SCTPPathVariables& rPath)
    : mIsEnabled(enable),
      mPeriodSecs(periodSecs),
      mBurst(burst),
      mGiveUpSecs(giveUpSecs),
      mrPath(rPath),
      mrAssoc(*rPath.association),
      mIsOn(false),
      mAlreadySent(0)
{
    const IPvXAddress& addr = rPath.remoteAddress;
    SCTPPathInfo* pinfo = new SCTPPathInfo("pinfo");
    pinfo->setRemoteAddress(addr);
    char str[128];

    snprintf(str, sizeof(str), "AP_PERIOD_TIMER %d:%s",
             mrAssoc.assocId,
             addr.str().c_str());
    mpPeriodTimer = new cMessage(str);
    mpPeriodTimer->setContextPointer(&mrAssoc);
    mpPeriodTimer->setControlInfo(pinfo->dup());

    snprintf(str, sizeof(str), "AP_GIVEUP_TIMER %d:%s",
             mrAssoc.assocId,
             addr.str().c_str());
    mpGiveUpTimer = new cMessage(str);
    mpGiveUpTimer->setContextPointer(&mrAssoc);
    mpGiveUpTimer->setControlInfo(pinfo->dup());

    delete pinfo;
}

SCTPAp::~SCTPAp()
{
    mrAssoc.stopTimer(mpPeriodTimer);
    delete mpPeriodTimer;
    mrAssoc.stopTimer(mpGiveUpTimer);
    delete mpGiveUpTimer;
}


bool
SCTPAp::TurnOnIfNeeded()
{
    // TODO check more activation conditions
    if (!mIsEnabled)
        return false;
    if (IsOn())
        return false;

    // send the first HEARTBEAT
    mrAssoc.sendHeartbeat(&mrPath);
    mAlreadySent++;

    // schedule the next PERIOD timer (for next HEARTBEAT)
    mrAssoc.startTimer(mpPeriodTimer, mPeriodSecs);
    // schedule the GIVEUP timer
    mrAssoc.startTimer(mpGiveUpTimer, mGiveUpSecs);

    mIsOn = true;
    EV << "---- SCTP-AP turned on on "
       << mrPath.remoteAddress.str().c_str() << endl;

    return true;
}

void
SCTPAp::TurnOff()
{
    if (!mIsEnabled)
        return;
    if (!mIsOn)
        return;

    mrAssoc.stopTimer(mpPeriodTimer);
    mrAssoc.stopTimer(mpGiveUpTimer);
    mAlreadySent = 0;
    mIsOn = false;
    EV << "---- SCTP-AP turned off on "
       << mrPath.remoteAddress.str().c_str() << std::endl;
}

void
SCTPAp::TurnOffOnAllPaths()
{
    if (!mIsEnabled)
        return;

    EV << "---- SCTP-AP is going to be turned off on all paths" << std::endl;
    SCTPAssociation::SCTPPathMap path_map = mrAssoc.sctpPathMap;
    for (SCTPAssociation::SCTPPathMap::iterator iterator = path_map.begin();
         iterator != path_map.end() ;
         ++iterator) {
        SCTPPathVariables* p_path = iterator->second;
        p_path->mpActiveProbing->TurnOff();
    }
}

bool
SCTPAp::IsOn()
{
    if (!mIsEnabled)
        return false;
    return mIsOn;
}

bool
SCTPAp::IsApTimer(const cMessage* const pMsg)
{
    if (pMsg == mpPeriodTimer)
        return true;
    if (pMsg == mpGiveUpTimer)
        return true;
    return false;
}

void
SCTPAp::ProcessTimeout(const cMessage* const pMsg)
{
    if (pMsg == mpPeriodTimer) {
        EV << "---- SCTP-AP PERIOD timeout on mAlreadySent="
           << mAlreadySent << ", Burst=" << mBurst << endl;
        mrAssoc.stopTimer(mpPeriodTimer);

        if (! mIsEnabled)
            return;
        if (mAlreadySent >= mBurst)
            return;

        EV << "---- SCTP-AP PERIOD timeout on "
           << mrPath.remoteAddress.str().c_str() << endl;
        mrAssoc.sendHeartbeat(&mrPath);
        mAlreadySent++;
        mrAssoc.startTimer(mpPeriodTimer, mPeriodSecs);
        return;
    }

    if (pMsg == mpGiveUpTimer) {
        EV << "---- SCTP-AP GIVEUP timeout on "
           << mrPath.remoteAddress.str().c_str() << endl;

        mrPath.mpActiveProbing->TurnOff();

        // This code is adapted SCTPAssociation::process_TIMEOUT_HEARTBEAT
        // there is also similar code in SCTPAssociation::process_TIMEOUT_RTX
        bool old_state;

        /* set path state to INACTIVE */
        old_state = mrPath.activePath;
        mrPath.activePath = false;
        SCTPPathVariables* p_next_path = mrAssoc.getNextPath(&mrPath);
        if (&mrPath == mrAssoc.state->getPrimaryPath()) {
            mrAssoc.state->setPrimaryPath(p_next_path);
        }
        sctpEV3 << "pathErrorCount now "<< mrPath.pathErrorCount
                << "; PP now " << mrAssoc.state->getPrimaryPathIndex() << endl;

        /* then: we can check, if all paths are INACTIVE ! */
        if (mrAssoc.allPathsInactive())
        {
            sctpEV3<<"sctp_do_hb_to_timer() : ALL PATHS INACTIVE --> closing ASSOC\n";
            mrAssoc.sendIndicationToApp(SCTP_I_CONN_LOST);
            return;
        } else if (mrPath.activePath == false && old_state == true)
        {
            /* notify the application, in case the PATH STATE has changed from ACTIVE to INACTIVE */
            mrAssoc.pathStatusIndication(&mrPath, false);
        }

        // Do Retransmission if any
        SCTPQueue* retrans_queue = mrAssoc.retransmissionQ;
        if (!retrans_queue->payloadQueue.empty()) {
            sctpEV3 << "Still " << retrans_queue->payloadQueue.size()
                    << " chunks in retrans_queue" << endl;

            for (SCTPQueue::PayloadQueue::iterator iterator = retrans_queue->payloadQueue.begin();
                 iterator != retrans_queue->payloadQueue.end(); iterator++) {
                SCTPDataVariables* chunk = iterator->second;

                // Only insert chunks that were sent to the path that has timed out
                if ( ((mrAssoc.chunkHasBeenAcked(chunk) == false && chunk->countsAsOutstanding) || chunk->hasBeenReneged) &&
                     (chunk->getLastDestinationPath() == &mrPath) ) {
                    sctpEV3 << simTime() << ": Timer-Based RTX for TSN "
                            << chunk->tsn << " on path " << chunk->getLastDestination() << endl;
                    chunk->getLastDestinationPath()->numberOfTimerBasedRetransmissions++;
                    SCTP::AssocStatMap::iterator iter = mrAssoc.sctpMain->assocStatMap.find(mrAssoc.assocId);
                    iter->second.numT3Rtx++;

                    mrAssoc.moveChunkToOtherPath(chunk, mrAssoc.getNextDestination(chunk));
                }
            }
        }

        // send pending data
        mrAssoc.sendOnAllPaths(p_next_path);
        return;
    }

    EV << "---- SCTP-AP asked to process unknown timer on "
       << mrPath.remoteAddress.str().c_str() << endl;
    return;
}