#include <fastrtps/rtps/flowcontrol/ThroughputController.h>
#include <fastrtps/rtps/resources/AsyncWriterThread.h>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;

namespace eprosima{
namespace fastrtps{
namespace rtps{

ThroughputController::ThroughputController(const ThroughputControllerDescriptor& descriptor, const RTPSWriter* associatedWriter):
   mSizeToClear(descriptor.sizeToClear),
   mAccumulatedPayloadSize(0),
   mRefreshTimeMS(descriptor.refreshTimeMS),
   mAssociatedParticipant(nullptr),
   mAssociatedWriter(associatedWriter)
{
}

ThroughputController::ThroughputController(const ThroughputControllerDescriptor& descriptor, const RTPSParticipantImpl* associatedParticipant):
   mSizeToClear(descriptor.sizeToClear),
   mAccumulatedPayloadSize(0),
   mRefreshTimeMS(descriptor.refreshTimeMS),
   mAssociatedParticipant(associatedParticipant),
   mAssociatedWriter(nullptr)
{
}

ThroughputControllerDescriptor::ThroughputControllerDescriptor(): sizeToClear(UINT32_MAX), refreshTimeMS(0)
{
}

ThroughputControllerDescriptor::ThroughputControllerDescriptor(uint32_t size, uint32_t time): sizeToClear(size), refreshTimeMS(time)
{
}

void ThroughputController::operator()(vector<CacheChangeForGroup_t>& changes)
{
   std::unique_lock<std::recursive_mutex> scopedLock(mThroughputControllerMutex);

   uint32_t accumulatedPayloadSizeBeforeControllering = mAccumulatedPayloadSize;
   unsigned int clearedChanges = 0;
   while (clearedChanges < changes.size())
   {
      auto& change = changes[clearedChanges];
      if (change.isFragmented())
      {
         unsigned int fittingFragments = min((mSizeToClear - mAccumulatedPayloadSize) / change.getChange()->getFragmentSize(),
                                              static_cast<uint32_t>(change.getFragmentsClearedForSending().set.size()));

         if (fittingFragments)
         {
            mAccumulatedPayloadSize += fittingFragments * change.getChange()->getFragmentSize();

            auto limitedFragments = change.getFragmentsClearedForSending();
            while (limitedFragments.set.size() > fittingFragments)
               limitedFragments.set.erase(std::prev(limitedFragments.set.end())); // remove biggest fragment

            change.setFragmentsClearedForSending(limitedFragments);
            clearedChanges++;
         }
         else
            break;
      }
      else
      {
         bool fits = (mAccumulatedPayloadSize + change.getChange()->serializedPayload.length) <= mSizeToClear;

         if (fits)
         {
            mAccumulatedPayloadSize += change.getChange()->serializedPayload.length;
            clearedChanges++;
         }
         else
            break;
      }
   }


   if (mAccumulatedPayloadSize != accumulatedPayloadSizeBeforeControllering)
      ScheduleRefresh(mAccumulatedPayloadSize - accumulatedPayloadSizeBeforeControllering);
   changes.erase(changes.begin() + clearedChanges, changes.end());
}

void ThroughputController::ScheduleRefresh(uint32_t sizeToRestore)
{
   shared_ptr<deadline_timer> throwawayTimer(make_shared<deadline_timer>(*FlowController::ControllerService));
   auto refresh = [throwawayTimer, this, sizeToRestore]
                   (const boost::system::error_code& error)
   {
      if ((error != boost::asio::error::operation_aborted) &&
          FlowController::IsListening(this))
      {
         std::unique_lock<std::recursive_mutex> scopedLock(mThroughputControllerMutex);
         throwawayTimer->cancel();
         mAccumulatedPayloadSize = sizeToRestore > mAccumulatedPayloadSize ? 0 : mAccumulatedPayloadSize - sizeToRestore;
         
         if (mAssociatedWriter)
            AsyncWriterThread::wakeUp(mAssociatedWriter);
         else if (mAssociatedParticipant)
            AsyncWriterThread::wakeUp(mAssociatedParticipant);
      }
   };

   throwawayTimer->expires_from_now(boost::posix_time::milliseconds(mRefreshTimeMS));
   throwawayTimer->async_wait(refresh);
}

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima