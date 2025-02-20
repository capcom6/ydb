#include "service.h"
#include <ydb/core/tx/conveyor/usage/service.h>
#include <ydb/core/kqp/query_data/kqp_predictor.h>

namespace NKikimr::NConveyor {

TDistributor::TDistributor(const TConfig& config, const TString& conveyorName, TIntrusivePtr<::NMonitoring::TDynamicCounters> conveyorSignals)
    : Config(config)
    , ConveyorName(conveyorName)
    , Counters(ConveyorName, conveyorSignals)
{

}

void TDistributor::Bootstrap() {
    const ui32 workersCount = Config.GetWorkersCountForConveyor(NKqp::TStagePredictor::GetUsableThreads());
    ALS_NOTICE(NKikimrServices::TX_CONVEYOR) << "action=conveyor_registered;actor_id=" << SelfId() << ";workers_count=" << workersCount << ";limit=" << Config.GetQueueSizeLimit();
    for (ui32 i = 0; i < workersCount; ++i) {
        Workers.emplace_back(Register(new TWorker(ConveyorName)));
    }
    Counters.AvailableWorkersCount->Set(Workers.size());
    Counters.WorkersCountLimit->Set(Workers.size());
    Counters.WaitingQueueSizeLimit->Set(Config.GetQueueSizeLimit());
    Become(&TDistributor::StateMain);
}

void TDistributor::HandleMain(TEvInternal::TEvTaskProcessedResult::TPtr& ev) {
    Counters.SolutionsRate->Inc();
    Counters.ExecuteHistogram->Collect((TMonotonic::Now() - ev->Get()->GetStartInstant()).MilliSeconds());
    if (Waiting.size()) {
        auto task = Waiting.pop();
        Counters.WaitingHistogram->Collect((TMonotonic::Now() - task.GetCreateInstant()).MilliSeconds());
        task.OnBeforeStart();
        Send(ev->Sender, new TEvInternal::TEvNewTask(task));
    } else {
        Workers.emplace_back(ev->Sender);
    }
    if (ev->Get()->IsFail()) {
        ALS_ERROR(NKikimrServices::TX_CONVEYOR) << "action=on_error;owner=" << ev->Get()->GetOwnerId() << ";workers=" << Workers.size() << ";waiting=" << Waiting.size();
        Send(ev->Get()->GetOwnerId(), new TEvExecution::TEvTaskProcessedResult(ev->Get()->GetError()));
    } else {
        Send(ev->Get()->GetOwnerId(), new TEvExecution::TEvTaskProcessedResult(ev->Get()->GetResult()));
    }
    Counters.WaitingQueueSize->Set(Waiting.size());
    Counters.AvailableWorkersCount->Set(Workers.size());
    ALS_DEBUG(NKikimrServices::TX_CONVEYOR) << "action=processed;owner=" << ev->Get()->GetOwnerId() << ";workers=" << Workers.size() << ";waiting=" << Waiting.size();
}

void TDistributor::HandleMain(TEvExecution::TEvNewTask::TPtr& ev) {
    ALS_DEBUG(NKikimrServices::TX_CONVEYOR) << "action=add_task;owner=" << ev->Sender << ";workers=" << Workers.size() << ";waiting=" << Waiting.size();
    Counters.IncomingRate->Inc();

    const TString taskClass = ev->Get()->GetTask()->GetTaskClassIdentifier();
    auto itSignal = Signals.find(taskClass);
    if (itSignal == Signals.end()) {
        itSignal = Signals.emplace(taskClass, std::make_shared<TTaskSignals>("Conveyor/" + ConveyorName, taskClass)).first;
    }

    TWorkerTask wTask(ev->Get()->GetTask(), ev->Get()->GetTask()->GetOwnerId().value_or(ev->Sender), itSignal->second);

    if (Workers.size()) {
        Counters.WaitingHistogram->Collect(0);

        wTask.OnBeforeStart();
        Send(Workers.back(), new TEvInternal::TEvNewTask(wTask));
        Workers.pop_back();
        Counters.UseWorkerRate->Inc();
    } else if (Waiting.size() < Config.GetQueueSizeLimit()) {
        Waiting.push(wTask);
        Counters.WaitWorkerRate->Inc();
    } else {
        ALS_ERROR(NKikimrServices::TX_CONVEYOR) << "action=overlimit;sender=" << ev->Sender << ";workers=" << Workers.size() << ";waiting=" << Waiting.size();
        Counters.OverlimitRate->Inc();
        Send(ev->Sender, new TEvExecution::TEvTaskProcessedResult(
            TConclusionStatus::Fail("scan conveyor overloaded (" + ::ToString(Waiting.size()) + " >= " + ::ToString(Config.GetQueueSizeLimit()) + ")")
        ));
    }
    Counters.WaitingQueueSize->Set(Waiting.size());
    Counters.AvailableWorkersCount->Set(Workers.size());
}

}
