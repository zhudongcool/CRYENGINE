// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "stdafx.h"
#include "ATL.h"
#include "AudioImpl.h"
#include "AudioCVars.h"
#include "ATLAudioObject.h"
#include "PoolObject_impl.h"
#include "Common/Logger.h"
#include "Managers.h"
#include "AudioObjectManager.h"
#include "AudioEventManager.h"
#include "FileCacheManager.h"
#include "AudioListenerManager.h"
#include "AudioEventListenerManager.h"
#include "AudioXMLProcessor.h"
#include "AudioStandaloneFileManager.h"
#include <CrySystem/ISystem.h>
#include <CryPhysics/IPhysics.h>
#include <CryEntitySystem/IEntitySystem.h>

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	#include "AudioSystem.h"
	#include "DebugColor.h"
	#include <CryRenderer/IRenderAuxGeom.h>
#endif // INCLUDE_AUDIO_PRODUCTION_CODE

namespace CryAudio
{
///////////////////////////////////////////////////////////////////////////
inline ERequestResult ConvertToRequestResult(ERequestStatus const eAudioRequestStatus)
{
	ERequestResult result;

	switch (eAudioRequestStatus)
	{
	case ERequestStatus::Success:
		{
			result = ERequestResult::Success;
			break;
		}
	case ERequestStatus::Failure:
	case ERequestStatus::FailureInvalidControlId:
	case ERequestStatus::FailureInvalidRequest:
	case ERequestStatus::PartialSuccess:
		{
			result = ERequestResult::Failure;
			break;
		}
	default:
		{
			Cry::Audio::Log(ELogType::Error, "Invalid AudioRequestStatus '%u'. Cannot be converted to an AudioRequestResult. ", eAudioRequestStatus);
			CRY_ASSERT(false);
			result = ERequestResult::Failure;
			break;
		}
	}

	return result;
}

//////////////////////////////////////////////////////////////////////////
CAudioTranslationLayer::~CAudioTranslationLayer()
{
	CRY_ASSERT_MESSAGE(g_pIImpl == nullptr, "<Audio> The implementation must get destroyed before the audio system is destructed.");
	CRY_ASSERT_MESSAGE(g_pObject == nullptr, "<Audio> The global object must get destroyed before the audio system is destructed.");
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::Initialize()
{
	if (g_cvars.m_audioObjectPoolSize < 1)
	{
		g_cvars.m_audioObjectPoolSize = 1;
		Cry::Audio::Log(ELogType::Warning, R"(Audio Object pool size must be at least 1. Forcing the cvar "s_AudioObjectPoolSize" to 1!)");
	}
	CATLAudioObject::CreateAllocator(g_cvars.m_audioObjectPoolSize);

	if (g_cvars.m_audioEventPoolSize < 1)
	{
		g_cvars.m_audioEventPoolSize = 1;
		Cry::Audio::Log(ELogType::Warning, R"(Audio Event pool size must be at least 1. Forcing the cvar "s_AudioEventPoolSize" to 1!)");
	}
	CATLEvent::CreateAllocator(g_cvars.m_audioEventPoolSize);

	if (g_cvars.m_audioStandaloneFilePoolSize < 1)
	{
		g_cvars.m_audioStandaloneFilePoolSize = 1;
		Cry::Audio::Log(ELogType::Warning, R"(Audio Standalone File pool size must be at least 1. Forcing the cvar "s_AudioStandaloneFilePoolSize" to 1!)");
	}
	CATLStandaloneFile::CreateAllocator(g_cvars.m_audioStandaloneFilePoolSize);

	// Add the callback for the obstruction calculation.
	gEnv->pPhysicalWorld->AddEventClient(
		EventPhysRWIResult::id,
		&CPropagationProcessor::OnObstructionTest,
		1);

	m_objectPoolSize = static_cast<uint32>(g_cvars.m_audioObjectPoolSize);
	m_eventPoolSize = static_cast<uint32>(g_cvars.m_audioEventPoolSize);

	g_objectManager.Initialize(m_objectPoolSize);
	g_eventManager.Initialize(m_eventPoolSize);
	g_fileCacheManager.Initialize();
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::Terminate()
{
	if (gEnv->pPhysicalWorld != nullptr)
	{
		// remove the callback for the obstruction calculation
		gEnv->pPhysicalWorld->RemoveEventClient(
			EventPhysRWIResult::id,
			&CPropagationProcessor::OnObstructionTest,
			1);
	}

	g_objectManager.Terminate();
	g_listenerManager.Terminate();
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::ProcessRequest(CAudioRequest& request)
{
	ERequestStatus result = ERequestStatus::None;

	if (request.GetData())
	{
		switch (request.GetData()->type)
		{
		case EAudioRequestType::AudioObjectRequest:
			{
				result = ProcessAudioObjectRequest(request);

				break;
			}
		case EAudioRequestType::AudioListenerRequest:
			{
				result = ProcessAudioListenerRequest(request.GetData());

				break;
			}
		case EAudioRequestType::AudioCallbackManagerRequest:
			{
				result = ProcessAudioCallbackManagerRequest(request);

				break;
			}
		case EAudioRequestType::AudioManagerRequest:
			{
				result = ProcessAudioManagerRequest(request);

				break;
			}
		default:
			{
				Cry::Audio::Log(ELogType::Error, "Unknown audio request type: %d", static_cast<int>(request.GetData()->type));
				CRY_ASSERT(false);

				break;
			}
		}
	}

	request.status = result;
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::Update(float const deltaTime)
{
	if (g_pIImpl != nullptr)
	{
		g_listenerManager.Update(deltaTime);
		g_pObject->GetImplDataPtr()->Update();
		g_objectManager.Update(deltaTime);
		g_pIImpl->Update();
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::NotifyListener(CAudioRequest const& request)
{
	ESystemEvents audioSystemEvent = ESystemEvents::None;
	CATLStandaloneFile* pStandaloneFile = nullptr;
	ControlId audioControlID = InvalidControlId;
	CATLEvent* pAudioEvent = nullptr;
	switch (request.GetData()->type)
	{
	case EAudioRequestType::AudioManagerRequest:
		{
			SAudioManagerRequestDataBase const* const pRequestDataBase = static_cast<SAudioManagerRequestDataBase const* const>(request.GetData());

			switch (pRequestDataBase->type)
			{
			case EAudioManagerRequestType::SetAudioImpl:
				audioSystemEvent = ESystemEvents::ImplSet;
				break;
			}

			break;
		}
	case EAudioRequestType::AudioCallbackManagerRequest:
		{
			SAudioCallbackManagerRequestDataBase const* const pRequestDataBase = static_cast<SAudioCallbackManagerRequestDataBase const* const>(request.GetData());

			switch (pRequestDataBase->type)
			{
			case EAudioCallbackManagerRequestType::ReportFinishedTriggerInstance:
				{
					SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportFinishedTriggerInstance> const* const pRequestData = static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportFinishedTriggerInstance> const* const>(pRequestDataBase);
					audioControlID = pRequestData->audioTriggerId;
					audioSystemEvent = ESystemEvents::TriggerFinished;

					break;
				}
			case EAudioCallbackManagerRequestType::ReportStartedEvent:
				{
					SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedEvent> const* const pRequestData = static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedEvent> const* const>(pRequestDataBase);
					pAudioEvent = &pRequestData->audioEvent;

					break;
				}
			case EAudioCallbackManagerRequestType::ReportStartedFile:
				{
					SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedFile> const* const pRequestData = static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedFile> const* const>(pRequestDataBase);
					pStandaloneFile = &pRequestData->audioStandaloneFile;
					audioSystemEvent = ESystemEvents::FileStarted;

					break;
				}
			case EAudioCallbackManagerRequestType::ReportStoppedFile:
				{
					SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStoppedFile> const* const pRequestData = static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStoppedFile> const* const>(pRequestDataBase);
					pStandaloneFile = &pRequestData->audioStandaloneFile;
					audioSystemEvent = ESystemEvents::FileStopped;

					break;
				}
			}

			break;
		}
	case EAudioRequestType::AudioObjectRequest:
		{
			SAudioObjectRequestDataBase const* const pRequestDataBase = static_cast<SAudioObjectRequestDataBase const* const>(request.GetData());

			switch (pRequestDataBase->type)
			{
			case EAudioObjectRequestType::ExecuteTrigger:
				{
					SAudioObjectRequestData<EAudioObjectRequestType::ExecuteTrigger> const* const pRequestData = static_cast<SAudioObjectRequestData<EAudioObjectRequestType::ExecuteTrigger> const* const>(pRequestDataBase);
					audioControlID = pRequestData->audioTriggerId;
					audioSystemEvent = ESystemEvents::TriggerExecuted;

					break;
				}
			case EAudioObjectRequestType::PlayFile:
				audioSystemEvent = ESystemEvents::FilePlay;
				break;
			}

			break;
		}
	case EAudioRequestType::AudioListenerRequest:
		{
			// Nothing to do currently for this type of request.

			break;
		}
	default:
		{
			CryFatalError("Unknown request type during CAudioTranslationLayer::NotifyListener!");

			break;
		}
	}

	SRequestInfo const requestInfo(
		ConvertToRequestResult(request.status),
		request.pOwner,
		request.pUserData,
		request.pUserDataOwner,
		audioSystemEvent,
		audioControlID,
		static_cast<IObject*>(request.pObject),
		pStandaloneFile,
		pAudioEvent);

	g_eventListenerManager.NotifyListener(&requestInfo);
}

///////////////////////////////////////////////////////////////////////////
ERequestStatus CAudioTranslationLayer::ProcessAudioManagerRequest(CAudioRequest const& request)
{
	ERequestStatus result = ERequestStatus::Failure;
	SAudioManagerRequestDataBase const* const pRequestDataBase = static_cast<SAudioManagerRequestDataBase const*>(request.GetData());

	switch (pRequestDataBase->type)
	{
	case EAudioManagerRequestType::AddRequestListener:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::AddRequestListener> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::AddRequestListener> const*>(request.GetData());
			result = g_eventListenerManager.AddRequestListener(pRequestData);

			break;
		}
	case EAudioManagerRequestType::RemoveRequestListener:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::RemoveRequestListener> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::RemoveRequestListener> const*>(request.GetData());
			result = g_eventListenerManager.RemoveRequestListener(pRequestData->func, pRequestData->pObjectToListenTo);

			break;
		}
	case EAudioManagerRequestType::SetAudioImpl:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::SetAudioImpl> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::SetAudioImpl> const*>(request.GetData());
			result = SetImpl(pRequestData->pIImpl);

			break;
		}
	case EAudioManagerRequestType::RefreshAudioSystem:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::RefreshAudioSystem> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::RefreshAudioSystem> const*>(request.GetData());
			result = RefreshAudioSystem(pRequestData->levelName);

			break;
		}
	case EAudioManagerRequestType::StopAllSounds:
		{
			result = g_pIImpl->StopAllSounds();

			break;
		}
	case EAudioManagerRequestType::ParseControlsData:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::ParseControlsData> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::ParseControlsData> const*>(request.GetData());
			g_xmlProcessor.ParseControlsData(pRequestData->folderPath.c_str(), pRequestData->dataScope);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::ParsePreloadsData:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::ParsePreloadsData> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::ParsePreloadsData> const*>(request.GetData());
			g_xmlProcessor.ParsePreloadsData(pRequestData->folderPath.c_str(), pRequestData->dataScope);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::ClearControlsData:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::ClearControlsData> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::ClearControlsData> const*>(request.GetData());
			g_xmlProcessor.ClearControlsData(pRequestData->dataScope);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::ClearPreloadsData:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::ClearPreloadsData> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::ClearPreloadsData> const*>(request.GetData());
			g_xmlProcessor.ClearPreloadsData(pRequestData->dataScope);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::PreloadSingleRequest:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::PreloadSingleRequest> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::PreloadSingleRequest> const*>(request.GetData());
			result = g_fileCacheManager.TryLoadRequest(pRequestData->audioPreloadRequestId, ((request.flags & ERequestFlags::ExecuteBlocking) != 0), pRequestData->bAutoLoadOnly);

			break;
		}
	case EAudioManagerRequestType::UnloadSingleRequest:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::UnloadSingleRequest> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::UnloadSingleRequest> const*>(request.GetData());
			result = g_fileCacheManager.TryUnloadRequest(pRequestData->audioPreloadRequestId);

			break;
		}
	case EAudioManagerRequestType::UnloadAFCMDataByScope:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::UnloadAFCMDataByScope> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::UnloadAFCMDataByScope> const*>(request.GetData());
			result = g_fileCacheManager.UnloadDataByScope(pRequestData->dataScope);

			break;
		}
	case EAudioManagerRequestType::ReleaseAudioImpl:
		{
			ReleaseImpl();
			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::ChangeLanguage:
		{
			SetImplLanguage();

			g_fileCacheManager.UpdateLocalizedFileCacheEntries();
			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::RetriggerAudioControls:
		{
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
			RetriggerAudioControls();
			result = ERequestStatus::Success;
#endif  // INCLUDE_AUDIO_PRODUCTION_CODE

			break;
		}
	case EAudioManagerRequestType::ReleasePendingRays:
		{
			g_objectManager.ReleasePendingRays();
			result = ERequestStatus::Success;

			break;
		}
	case EAudioManagerRequestType::ReloadControlsData:
		{
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
			SAudioManagerRequestData<EAudioManagerRequestType::ReloadControlsData> const* const pRequestData = static_cast<SAudioManagerRequestData<EAudioManagerRequestType::ReloadControlsData> const*>(request.GetData());

			for (auto const pObject : g_objectManager.GetObjects())
			{
				for (auto const pEvent : pObject->GetActiveEvents())
				{
					if (pEvent != nullptr)
					{
						pEvent->Release();
					}
				}
			}

			g_xmlProcessor.ClearControlsData(EDataScope::All);
			g_xmlProcessor.ParseControlsData(pRequestData->folderPath, EDataScope::Global);

			if (strcmp(pRequestData->levelName, "") != 0)
			{
				g_xmlProcessor.ParseControlsData(pRequestData->folderPath + pRequestData->levelName, EDataScope::LevelSpecific);
			}

			RetriggerAudioControls();

			result = ERequestStatus::Success;
#endif  // INCLUDE_AUDIO_PRODUCTION_CODE
			break;
		}
	case EAudioManagerRequestType::DrawDebugInfo:
		{
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
			DrawAudioSystemDebugInfo();
			result = ERequestStatus::Success;
#endif  // INCLUDE_AUDIO_PRODUCTION_CODE

			break;
		}
	case EAudioManagerRequestType::GetAudioFileData:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::GetAudioFileData> const* const pRequestData =
				static_cast<SAudioManagerRequestData<EAudioManagerRequestType::GetAudioFileData> const* const>(request.GetData());
			g_pIImpl->GetFileData(pRequestData->name.c_str(), pRequestData->fileData);
			break;
		}
	case EAudioManagerRequestType::GetImplInfo:
		{
			SAudioManagerRequestData<EAudioManagerRequestType::GetImplInfo> const* const pRequestData =
				static_cast<SAudioManagerRequestData<EAudioManagerRequestType::GetImplInfo> const* const>(request.GetData());
			g_pIImpl->GetInfo(pRequestData->implInfo);
			break;
		}
	case EAudioManagerRequestType::None:
		{
			result = ERequestStatus::Success;

			break;
		}
	default:
		{
			Cry::Audio::Log(ELogType::Warning, "ATL received an unknown AudioManager request: %u", pRequestDataBase->type);
			result = ERequestStatus::FailureInvalidRequest;

			break;
		}
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////
ERequestStatus CAudioTranslationLayer::ProcessAudioCallbackManagerRequest(CAudioRequest& request)
{
	ERequestStatus result = ERequestStatus::Failure;
	SAudioCallbackManagerRequestDataBase const* const pRequestDataBase =
		static_cast<SAudioCallbackManagerRequestDataBase const* const>(request.GetData());

	switch (pRequestDataBase->type)
	{
	case EAudioCallbackManagerRequestType::ReportStartedEvent:
		{
			SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedEvent> const* const pRequestData =
				static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedEvent> const* const>(request.GetData());
			CATLEvent& audioEvent = pRequestData->audioEvent;

			audioEvent.m_state = EEventState::PlayingDelayed;

			if (audioEvent.m_pAudioObject != g_pObject)
			{
				g_objectManager.ReportStartedEvent(&audioEvent);
			}
			else
			{
				g_pObject->ReportStartedEvent(&audioEvent);
			}

			result = ERequestStatus::Success;

			break;
		}
	case EAudioCallbackManagerRequestType::ReportFinishedEvent:
		{
			SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportFinishedEvent> const* const pRequestData =
				static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportFinishedEvent> const* const>(request.GetData());
			CATLEvent& event = pRequestData->audioEvent;

			if (event.m_pAudioObject != g_pObject)
			{
				g_objectManager.ReportFinishedEvent(&event, pRequestData->bSuccess);
			}
			else
			{
				g_pObject->ReportFinishedEvent(&event, pRequestData->bSuccess);
			}

			g_eventManager.DestructEvent(&event);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioCallbackManagerRequestType::ReportVirtualizedEvent:
		{
			SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportVirtualizedEvent> const* const pRequestData =
				static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportVirtualizedEvent> const* const>(request.GetData());

			pRequestData->audioEvent.m_state = EEventState::Virtual;
			CATLAudioObject* const pObject = pRequestData->audioEvent.m_pAudioObject;

			if ((pObject->GetFlags() & EObjectFlags::Virtual) == 0)
			{
				bool isVirtual = true;

				for (auto const pEvent : pObject->GetActiveEvents())
				{
					if (pEvent->m_state != EEventState::Virtual)
					{
						isVirtual = false;
						break;
					}
				}

				if (isVirtual)
				{
					pObject->SetFlag(EObjectFlags::Virtual);

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
					pObject->ResetObstructionRays();
#endif      // INCLUDE_AUDIO_PRODUCTION_CODE
				}
			}

			result = ERequestStatus::Success;

			break;
		}
	case EAudioCallbackManagerRequestType::ReportPhysicalizedEvent:
		{
			SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportPhysicalizedEvent> const* const pRequestData =
				static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportPhysicalizedEvent> const* const>(request.GetData());

			pRequestData->audioEvent.m_state = EEventState::Playing;
			pRequestData->audioEvent.m_pAudioObject->RemoveFlag(EObjectFlags::Virtual);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioCallbackManagerRequestType::ReportStartedFile:
		{
			SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedFile> const* const pRequestData =
				static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStartedFile> const* const>(request.GetData());

			CATLStandaloneFile& audioStandaloneFile = pRequestData->audioStandaloneFile;

			g_objectManager.GetStartedStandaloneFileRequestData(&audioStandaloneFile, request);
			audioStandaloneFile.m_state = (pRequestData->bSuccess) ? EAudioStandaloneFileState::Playing : EAudioStandaloneFileState::None;

			result = (pRequestData->bSuccess) ? ERequestStatus::Success : ERequestStatus::Failure;

			break;
		}
	case EAudioCallbackManagerRequestType::ReportStoppedFile:
		{
			SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStoppedFile> const* const pRequestData =
				static_cast<SAudioCallbackManagerRequestData<EAudioCallbackManagerRequestType::ReportStoppedFile> const* const>(request.GetData());

			CATLStandaloneFile& audioStandaloneFile = pRequestData->audioStandaloneFile;

			g_objectManager.GetStartedStandaloneFileRequestData(&audioStandaloneFile, request);

			if (audioStandaloneFile.m_pAudioObject != g_pObject)
			{
				g_objectManager.ReportFinishedStandaloneFile(&audioStandaloneFile);
			}
			else
			{
				g_pObject->ReportFinishedStandaloneFile(&audioStandaloneFile);
			}

			g_fileManager.ReleaseStandaloneFile(&audioStandaloneFile);

			result = ERequestStatus::Success;

			break;
		}
	case EAudioCallbackManagerRequestType::ReportFinishedTriggerInstance:
	case EAudioCallbackManagerRequestType::None:
		{
			result = ERequestStatus::Success;

			break;
		}
	default:
		{
			result = ERequestStatus::FailureInvalidRequest;
			Cry::Audio::Log(ELogType::Warning, "ATL received an unknown AudioCallbackManager request: %u", pRequestDataBase->type);

			break;
		}
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////
ERequestStatus CAudioTranslationLayer::ProcessAudioObjectRequest(CAudioRequest const& request)
{
	ERequestStatus result = ERequestStatus::Failure;
	CATLAudioObject* const pObject = (request.pObject != nullptr) ? request.pObject : g_pObject;

	SAudioObjectRequestDataBase const* const pBaseRequestData =
		static_cast<SAudioObjectRequestDataBase const* const>(request.GetData());

	switch (pBaseRequestData->type)
	{
	case EAudioObjectRequestType::LoadTrigger:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::LoadTrigger> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::LoadTrigger> const* const>(request.GetData());

			CTrigger const* const pTrigger = stl::find_in_map(g_triggers, pRequestData->audioTriggerId, nullptr);

			if (pTrigger != nullptr)
			{
				pTrigger->LoadAsync(*pObject, true);
				result = ERequestStatus::Success;
			}
			else
			{
				result = ERequestStatus::FailureInvalidControlId;
			}

			break;
		}
	case EAudioObjectRequestType::UnloadTrigger:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::UnloadTrigger> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::UnloadTrigger> const* const>(request.GetData());

			CTrigger const* const pTrigger = stl::find_in_map(g_triggers, pRequestData->audioTriggerId, nullptr);

			if (pTrigger != nullptr)
			{
				pTrigger->LoadAsync(*pObject, false);
				result = ERequestStatus::Success;
			}
			else
			{
				result = ERequestStatus::FailureInvalidControlId;
			}

			break;
		}
	case EAudioObjectRequestType::PlayFile:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::PlayFile> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::PlayFile> const* const>(request.GetData());

			if (pRequestData != nullptr && !pRequestData->file.empty())
			{
				if (pRequestData->usedAudioTriggerId != InvalidControlId)
				{
					CTrigger const* const pTrigger = stl::find_in_map(g_triggers, pRequestData->usedAudioTriggerId, nullptr);

					if (pTrigger != nullptr)
					{
						pTrigger->PlayFile(
							*pObject,
							pRequestData->file.c_str(),
							pRequestData->bLocalized,
							request.pOwner,
							request.pUserData,
							request.pUserDataOwner);
					}
				}

				result = ERequestStatus::Success;
			}

			break;
		}
	case EAudioObjectRequestType::StopFile:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::StopFile> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::StopFile> const* const>(request.GetData());

			if (pRequestData != nullptr && !pRequestData->file.empty())
			{
				pObject->HandleStopFile(pRequestData->file.c_str());
				result = ERequestStatus::Success;
			}

			break;
		}
	case EAudioObjectRequestType::ExecuteTrigger:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::ExecuteTrigger> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::ExecuteTrigger> const* const>(request.GetData());

			CTrigger const* const pTrigger = stl::find_in_map(g_triggers, pRequestData->audioTriggerId, nullptr);

			if (pTrigger != nullptr)
			{
				pTrigger->Execute(*pObject, request.pOwner, request.pUserData, request.pUserDataOwner, request.flags);
				result = ERequestStatus::Success;
			}
			else
			{
				result = ERequestStatus::FailureInvalidControlId;
			}

			break;
		}
	case EAudioObjectRequestType::ExecuteTriggerEx:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::ExecuteTriggerEx> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::ExecuteTriggerEx> const* const>(request.GetData());

			CTrigger const* const pTrigger = stl::find_in_map(g_triggers, pRequestData->triggerId, nullptr);

			if (pTrigger != nullptr)
			{
				auto const pNewObject = new CATLAudioObject;
				g_objectManager.RegisterObject(pNewObject);

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
				pNewObject->Init(pRequestData->name.c_str(), g_pIImpl->ConstructObject(pRequestData->name.c_str()), pRequestData->entityId);
#else
				pNewObject->Init(nullptr, g_pIImpl->ConstructObject(nullptr), pRequestData->entityId);
#endif    // INCLUDE_AUDIO_PRODUCTION_CODE

				pNewObject->HandleSetTransformation(pRequestData->transformation, 0.0f);

				if (pRequestData->setCurrentEnvironments)
				{
					SetCurrentEnvironmentsOnObject(pNewObject, INVALID_ENTITYID);
				}

				SetOcclusionType(*pNewObject, pRequestData->occlusionType);
				pTrigger->Execute(*pNewObject, request.pOwner, request.pUserData, request.pUserDataOwner, request.flags);
				pNewObject->RemoveFlag(EObjectFlags::InUse);
				result = ERequestStatus::Success;
			}
			else
			{
				result = ERequestStatus::FailureInvalidControlId;
			}

			break;
		}
	case EAudioObjectRequestType::StopTrigger:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::StopTrigger> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::StopTrigger> const* const>(request.GetData());

			CTrigger const* const pTrigger = stl::find_in_map(g_triggers, pRequestData->audioTriggerId, nullptr);

			if (pTrigger != nullptr)
			{
				result = pObject->HandleStopTrigger(pTrigger);
			}
			else
			{
				result = ERequestStatus::FailureInvalidControlId;
			}

			break;
		}
	case EAudioObjectRequestType::StopAllTriggers:
		{
			pObject->StopAllTriggers();
			result = ERequestStatus::Success;

			break;
		}
	case EAudioObjectRequestType::SetTransformation:
		{
			CRY_ASSERT_MESSAGE(pObject != g_pObject, "Received a request to set a transformation on the global object.");

			float const distanceToListener = (g_listenerManager.GetActiveListenerTransformation().GetPosition() - pObject->GetTransformation().GetPosition()).GetLength();
			SAudioObjectRequestData<EAudioObjectRequestType::SetTransformation> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetTransformation> const* const>(request.GetData());

			pObject->HandleSetTransformation(pRequestData->transformation, distanceToListener);
			result = ERequestStatus::Success;

			break;
		}
	case EAudioObjectRequestType::SetParameter:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::SetParameter> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetParameter> const* const>(request.GetData());

			CParameter const* const pParameter = stl::find_in_map(g_parameters, pRequestData->parameterId, nullptr);

			if (pParameter != nullptr)
			{
				pParameter->Set(*pObject, pRequestData->value);
				result = ERequestStatus::Success;
			}
			else
			{
				result = ERequestStatus::FailureInvalidControlId;
			}

			break;
		}
	case EAudioObjectRequestType::SetSwitchState:
		{
			result = ERequestStatus::FailureInvalidControlId;
			SAudioObjectRequestData<EAudioObjectRequestType::SetSwitchState> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetSwitchState> const* const>(request.GetData());

			CATLSwitch const* const pSwitch = stl::find_in_map(g_switches, pRequestData->audioSwitchId, nullptr);

			if (pSwitch != nullptr)
			{
				CATLSwitchState const* const pState = stl::find_in_map(pSwitch->GetStates(), pRequestData->audioSwitchStateId, nullptr);

				if (pState != nullptr)
				{
					pState->Set(*pObject);
					result = ERequestStatus::Success;
				}
			}

			break;
		}
	case EAudioObjectRequestType::SetOcclusionType:
		{
			CRY_ASSERT_MESSAGE(pObject != g_pObject, "Received a request to set the occlusion type on the global object.");

			SAudioObjectRequestData<EAudioObjectRequestType::SetOcclusionType> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetOcclusionType> const* const>(request.GetData());

			SetOcclusionType(*pObject, pRequestData->occlusionType);
			result = ERequestStatus::Success;

			break;
		}
	case EAudioObjectRequestType::SetCurrentEnvironments:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::SetCurrentEnvironments> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetCurrentEnvironments> const* const>(request.GetData());

			SetCurrentEnvironmentsOnObject(pObject, pRequestData->entityToIgnore);

			break;
		}
	case EAudioObjectRequestType::SetEnvironment:
		{
			if (pObject != g_pObject)
			{
				SAudioObjectRequestData<EAudioObjectRequestType::SetEnvironment> const* const pRequestData =
					static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetEnvironment> const* const>(request.GetData());

				CATLAudioEnvironment const* const pEnvironment = stl::find_in_map(g_environments, pRequestData->audioEnvironmentId, nullptr);

				if (pEnvironment != nullptr)
				{
					pObject->HandleSetEnvironment(pEnvironment, pRequestData->amount);
					result = ERequestStatus::Success;
				}
				else
				{
					result = ERequestStatus::FailureInvalidControlId;
				}
			}
			else
			{
				Cry::Audio::Log(ELogType::Warning, "ATL received a request to set an environment on a global object");
			}

			break;
		}
	case EAudioObjectRequestType::RegisterObject:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::RegisterObject> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::RegisterObject> const*>(request.GetData());

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
			pObject->Init(pRequestData->name.c_str(), g_pIImpl->ConstructObject(pRequestData->name.c_str()), pRequestData->entityId);
#else
			pObject->Init(nullptr, g_pIImpl->ConstructObject(nullptr), pRequestData->entityId);
#endif  // INCLUDE_AUDIO_PRODUCTION_CODE

			pObject->HandleSetTransformation(pRequestData->transformation, 0.0f);

			if (pRequestData->setCurrentEnvironments)
			{
				SetCurrentEnvironmentsOnObject(pObject, INVALID_ENTITYID);
			}

			SetOcclusionType(*pObject, pRequestData->occlusionType);
			g_objectManager.RegisterObject(pObject);
			result = ERequestStatus::Success;

			break;
		}
	case EAudioObjectRequestType::ReleaseObject:
		{
			if (pObject != g_pObject)
			{
				pObject->RemoveFlag(EObjectFlags::InUse);
				result = ERequestStatus::Success;
			}
			else
			{
				Cry::Audio::Log(ELogType::Warning, "ATL received a request to release the GlobalAudioObject");
			}

			break;
		}
	case EAudioObjectRequestType::ProcessPhysicsRay:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::ProcessPhysicsRay> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::ProcessPhysicsRay> const* const>(request.GetData());

			pObject->ProcessPhysicsRay(pRequestData->pAudioRayInfo);
			result = ERequestStatus::Success;
			break;
		}
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	case EAudioObjectRequestType::SetName:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::SetName> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::SetName> const* const>(request.GetData());

			result = pObject->HandleSetName(pRequestData->name.c_str());

			if (result == ERequestStatus::SuccessNeedsRefresh)
			{
				pObject->ForceImplementationRefresh(true);
				result = ERequestStatus::Success;
			}

			break;
		}
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
	case EAudioObjectRequestType::ToggleAbsoluteVelocityTracking:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::ToggleAbsoluteVelocityTracking> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::ToggleAbsoluteVelocityTracking> const* const>(request.GetData());

			if (pRequestData->isEnabled)
			{
				pObject->SetFlag(EObjectFlags::TrackAbsoluteVelocity);
			}
			else
			{
				pObject->RemoveFlag(EObjectFlags::TrackAbsoluteVelocity);
			}

			result = ERequestStatus::Success;
			break;
		}
	case EAudioObjectRequestType::ToggleRelativeVelocityTracking:
		{
			SAudioObjectRequestData<EAudioObjectRequestType::ToggleRelativeVelocityTracking> const* const pRequestData =
				static_cast<SAudioObjectRequestData<EAudioObjectRequestType::ToggleRelativeVelocityTracking> const* const>(request.GetData());

			if (pRequestData->isEnabled)
			{
				pObject->SetFlag(EObjectFlags::TrackRelativeVelocity);
			}
			else
			{
				pObject->RemoveFlag(EObjectFlags::TrackRelativeVelocity);
			}

			result = ERequestStatus::Success;
			break;
		}
	case EAudioObjectRequestType::None:
		{
			result = ERequestStatus::Success;
			break;
		}
	default:
		{
			Cry::Audio::Log(ELogType::Warning, "ATL received an unknown AudioObject request type: %u", pBaseRequestData->type);
			result = ERequestStatus::FailureInvalidRequest;
			break;
		}
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////
ERequestStatus CAudioTranslationLayer::ProcessAudioListenerRequest(SAudioRequestData const* const pPassedRequestData)
{
	ERequestStatus result = ERequestStatus::Failure;
	SAudioListenerRequestDataBase const* const pBaseRequestData =
		static_cast<SAudioListenerRequestDataBase const* const>(pPassedRequestData);

	switch (pBaseRequestData->type)
	{
	case EAudioListenerRequestType::SetTransformation:
		{
			SAudioListenerRequestData<EAudioListenerRequestType::SetTransformation> const* const pRequestData =
				static_cast<SAudioListenerRequestData<EAudioListenerRequestType::SetTransformation> const* const>(pPassedRequestData);

			CRY_ASSERT(pRequestData->pListener != nullptr);

			if (pRequestData->pListener != nullptr)
			{
				pRequestData->pListener->HandleSetTransformation(pRequestData->transformation);
			}

			result = ERequestStatus::Success;
		}
		break;
	case EAudioListenerRequestType::RegisterListener:
		{
			SAudioListenerRequestData<EAudioListenerRequestType::RegisterListener> const* const pRequestData =
				static_cast<SAudioListenerRequestData<EAudioListenerRequestType::RegisterListener> const*>(pPassedRequestData);
			*pRequestData->ppListener = g_listenerManager.CreateListener(pRequestData->name.c_str());
		}
		break;
	case EAudioListenerRequestType::ReleaseListener:
		{
			SAudioListenerRequestData<EAudioListenerRequestType::ReleaseListener> const* const pRequestData =
				static_cast<SAudioListenerRequestData<EAudioListenerRequestType::ReleaseListener> const* const>(pPassedRequestData);

			CRY_ASSERT(pRequestData->pListener != nullptr);

			if (pRequestData->pListener != nullptr)
			{
				g_listenerManager.ReleaseListener(pRequestData->pListener);
				result = ERequestStatus::Success;
			}
		}
		break;
#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
	case EAudioListenerRequestType::SetName:
		{
			SAudioListenerRequestData<EAudioListenerRequestType::SetName> const* const pRequestData =
				static_cast<SAudioListenerRequestData<EAudioListenerRequestType::SetName> const* const>(pPassedRequestData);

			pRequestData->pListener->HandleSetName(pRequestData->name.c_str());
			result = ERequestStatus::Success;
		}
		break;
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
	case EAudioListenerRequestType::None:
		result = ERequestStatus::Success;
		break;
	default:
		result = ERequestStatus::FailureInvalidRequest;
		Cry::Audio::Log(ELogType::Warning, "ATL received an unknown AudioListener request type: %u", pPassedRequestData->type);
		break;
	}

	return result;
}
///////////////////////////////////////////////////////////////////////////
ERequestStatus CAudioTranslationLayer::SetImpl(Impl::IImpl* const pIImpl)
{
	ERequestStatus result = ERequestStatus::Failure;

	if (g_pIImpl != nullptr && pIImpl != g_pIImpl)
	{
		ReleaseImpl();
	}

	g_pIImpl = pIImpl;

	if (g_pIImpl == nullptr)
	{
		Cry::Audio::Log(ELogType::Warning, "nullptr passed to SetImpl, will run with the null implementation");

		auto const pImpl = new Impl::Null::CImpl();
		CRY_ASSERT(pImpl != nullptr);
		g_pIImpl = static_cast<Impl::IImpl*>(pImpl);
	}

	result = g_pIImpl->Init(m_objectPoolSize, m_eventPoolSize);

	g_pIImpl->GetInfo(m_implInfo);
	m_configPath = AUDIO_SYSTEM_DATA_ROOT "/";
	m_configPath += (m_implInfo.folderName + "/" + s_szConfigFolderName + "/").c_str();

	if (result != ERequestStatus::Success)
	{
		// The impl failed to initialize, allow it to shut down and release then fall back to the null impl.
		Cry::Audio::Log(ELogType::Error, "Failed to set the AudioImpl %s. Will run with the null implementation.", m_implInfo.name.c_str());

		// There's no need to call Shutdown when the initialization failed as
		// we expect the implementation to clean-up itself if it couldn't be initialized

		g_pIImpl->Release(); // Release the engine specific data.

		auto const pImpl = new Impl::Null::CImpl();
		CRY_ASSERT(pImpl != nullptr);
		g_pIImpl = static_cast<Impl::IImpl*>(pImpl);
	}

	if (g_pObject == nullptr)
	{
		g_pObject = new CATLAudioObject;

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
		g_pObject->m_name = "Global Object";
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
	}

	CRY_ASSERT_MESSAGE(g_pObject->GetImplDataPtr() == nullptr, "<Audio> The global object's impl-data must be nullptr during initialization.");
	g_pObject->SetImplDataPtr(g_pIImpl->ConstructGlobalObject());

	g_objectManager.OnAfterImplChanged();
	g_eventManager.OnAfterImplChanged();
	g_listenerManager.OnAfterImplChanged();

	SetImplLanguage();

	return result;
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::ReleaseImpl()
{
	// Reject new requests during shutdown.
	m_flags |= EInternalStates::AudioMiddlewareShuttingDown;

	// Release middleware specific data before its shutdown.
	g_fileManager.ReleaseImplData();
	g_listenerManager.ReleaseImplData();
	g_eventManager.ReleaseImplData();
	g_objectManager.ReleaseImplData();

	g_pIImpl->DestructObject(g_pObject->GetImplDataPtr());
	g_pObject->SetImplDataPtr(nullptr);

	delete g_pObject;
	g_pObject = nullptr;

	g_xmlProcessor.ClearPreloadsData(EDataScope::All);
	g_xmlProcessor.ClearControlsData(EDataScope::All);

	g_pIImpl->ShutDown();
	g_pIImpl->Release();
	g_pIImpl = nullptr;

	// Release engine specific data after impl shut down to prevent dangling data accesses during shutdown.
	// Note: The object and listener managers are an exception as we need their data to survive in case the middleware is swapped out.
	g_eventManager.Release();
	g_fileManager.Release();

	m_flags &= ~EInternalStates::AudioMiddlewareShuttingDown;
}

//////////////////////////////////////////////////////////////////////////
ERequestStatus CAudioTranslationLayer::RefreshAudioSystem(char const* const szLevelName)
{
	Cry::Audio::Log(ELogType::Warning, "Beginning to refresh the AudioSystem!");

	ERequestStatus result = g_pIImpl->StopAllSounds();
	CRY_ASSERT(result == ERequestStatus::Success);

	result = g_fileCacheManager.UnloadDataByScope(EDataScope::LevelSpecific);
	CRY_ASSERT(result == ERequestStatus::Success);

	result = g_fileCacheManager.UnloadDataByScope(EDataScope::Global);
	CRY_ASSERT(result == ERequestStatus::Success);

	g_xmlProcessor.ClearPreloadsData(EDataScope::All);
	g_xmlProcessor.ClearControlsData(EDataScope::All);

	g_pIImpl->OnRefresh();

	g_xmlProcessor.ParseControlsData(m_configPath.c_str(), EDataScope::Global);
	g_xmlProcessor.ParsePreloadsData(m_configPath.c_str(), EDataScope::Global);

	// The global preload might not exist if no preloads have been created, for that reason we don't check the result of this call
	result = g_fileCacheManager.TryLoadRequest(GlobalPreloadRequestId, true, true);

	if (szLevelName != nullptr && szLevelName[0] != '\0')
	{
		CryFixedStringT<MaxFilePathLength> levelPath = m_configPath;
		levelPath += s_szLevelsFolderName;
		levelPath += "/";
		levelPath += szLevelName;
		g_xmlProcessor.ParseControlsData(levelPath.c_str(), EDataScope::LevelSpecific);
		g_xmlProcessor.ParsePreloadsData(levelPath.c_str(), EDataScope::LevelSpecific);

		PreloadRequestId const preloadRequestId = StringToId(szLevelName);
		result = g_fileCacheManager.TryLoadRequest(preloadRequestId, true, true);

		if (result != ERequestStatus::Success)
		{
			Cry::Audio::Log(ELogType::Warning, R"(No preload request found for level - "%s"!)", szLevelName);
		}
	}

	Cry::Audio::Log(ELogType::Warning, "Done refreshing the AudioSystem!");

	return ERequestStatus::Success;
}

//////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::SetImplLanguage()
{
	if (ICVar* pCVar = gEnv->pConsole->GetCVar("g_languageAudio"))
	{
		g_pIImpl->SetLanguage(pCVar->GetString());
	}
}

//////////////////////////////////////////////////////////////////////////
bool CAudioTranslationLayer::OnInputEvent(SInputEvent const& event)
{
	if (event.state == eIS_Changed && event.deviceType == eIDT_Gamepad)
	{
		if (event.keyId == eKI_SYS_ConnectDevice)
		{
			g_pIImpl->GamepadConnected(event.deviceUniqueID);
		}
		else if (event.keyId == eKI_SYS_DisconnectDevice)
		{
			g_pIImpl->GamepadDisconnected(event.deviceUniqueID);
		}
	}

	// Do not consume event
	return false;
}

//////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::SetCurrentEnvironmentsOnObject(CATLAudioObject* const pObject, EntityId const entityToIgnore)
{
	IAreaManager* const pIAreaManager = gEnv->pEntitySystem->GetAreaManager();
	size_t numAreas = 0;
	static size_t const s_maxAreas = 10;
	static SAudioAreaInfo s_areaInfos[s_maxAreas];

	if (pIAreaManager->QueryAudioAreas(pObject->GetTransformation().GetPosition(), s_areaInfos, s_maxAreas, numAreas))
	{
		for (size_t i = 0; i < numAreas; ++i)
		{
			SAudioAreaInfo const& areaInfo = s_areaInfos[i];

			if (entityToIgnore == INVALID_ENTITYID || entityToIgnore != areaInfo.envProvidingEntityId)
			{
				CATLAudioEnvironment const* const pEnvironment = stl::find_in_map(g_environments, areaInfo.audioEnvironmentId, nullptr);

				if (pEnvironment != nullptr)
				{
					pObject->HandleSetEnvironment(pEnvironment, areaInfo.amount);
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::SetOcclusionType(CATLAudioObject& object, EOcclusionType const occlusionType) const
{
	switch (occlusionType)
	{
	case EOcclusionType::Ignore:
		{
			object.HandleSetOcclusionType(EOcclusionType::Ignore);
			object.SetObstructionOcclusion(0.0f, 0.0f);

			break;
		}
	case EOcclusionType::Adaptive:
		{
			object.HandleSetOcclusionType(EOcclusionType::Adaptive);

			break;
		}
	case EOcclusionType::Low:
		{
			object.HandleSetOcclusionType(EOcclusionType::Low);

			break;
		}
	case EOcclusionType::Medium:
		{
			object.HandleSetOcclusionType(EOcclusionType::Medium);

			break;
		}
	case EOcclusionType::High:
		{
			object.HandleSetOcclusionType(EOcclusionType::High);

			break;
		}
	default:
		{
			Cry::Audio::Log(ELogType::Warning, "Unknown occlusion type during SetOcclusionType: %u", occlusionType);

			break;
		}
	}
}

//////////////////////////////////////////////////////////////////////////
char const* CAudioTranslationLayer::GetConfigPath() const
{
	return m_configPath.c_str();
}

#if defined(INCLUDE_AUDIO_PRODUCTION_CODE)
//////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::DrawAudioSystemDebugInfo()
{
	CRY_PROFILE_FUNCTION(PROFILE_AUDIO);
	IRenderAuxGeom* pAuxGeom = gEnv->pRenderer ? gEnv->pRenderer->GetOrCreateIRenderAuxGeom() : nullptr;
	if (pAuxGeom)
	{
		if ((g_cvars.m_drawAudioDebug & Debug::objectMask) != 0)
		{
			DrawAudioObjectDebugInfo(*pAuxGeom); // needs to be called first so that the rest of the labels are printed
			// on top (Draw2dLabel doesn't provide a way set which labels are printed on top)
		}

		float posX = 8.0f;
		float posY = 4.0f;

		if (!((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::HideMemoryInfo) != 0))
		{
			pAuxGeom->Draw2dLabel(posX, posY, 1.5f, Debug::g_systemColorHeader.data(), false, m_implInfo.name.c_str());

			CryModuleMemoryInfo memInfo;
			ZeroStruct(memInfo);
			CryGetMemoryInfoForModule(&memInfo);

			posY += Debug::g_systemLineHeightClause;
			pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextPrimary.data(), false,
			                      "[Audio System] Total Memory Used: %uKiB",
			                      static_cast<uint32>((memInfo.allocated - memInfo.freed) / 1024));

			posX += Debug::g_systemIndentation;
			{
				CPoolObject<CATLAudioObject, stl::PSyncMultiThread>::Allocator& allocator = CATLAudioObject::GetAllocator();
				posY += Debug::g_systemLineHeight;
				auto mem = allocator.GetTotalMemory();
				auto pool = allocator.GetCounts();
				pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextSecondary.data(), false, "[Objects] InUse: %u | Constructed: %u (%uKiB) | Memory Pool: %uKiB", pool.nUsed, pool.nAlloc, mem.nUsed / 1024, mem.nAlloc / 1024);
			}

			{
				CPoolObject<CATLEvent, stl::PSyncNone>::Allocator& allocator = CATLEvent::GetAllocator();
				posY += Debug::g_systemLineHeight;
				auto mem = allocator.GetTotalMemory();
				auto pool = allocator.GetCounts();
				pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextSecondary.data(), false, "[Events] InUse: %u | Constructed: %u (%uKiB) | Memory Pool: %uKiB", pool.nUsed, pool.nAlloc, mem.nUsed / 1024, mem.nAlloc / 1024);
			}

			{
				CPoolObject<CATLStandaloneFile, stl::PSyncNone>::Allocator& allocator = CATLStandaloneFile::GetAllocator();
				posY += Debug::g_systemLineHeight;
				auto mem = allocator.GetTotalMemory();
				auto pool = allocator.GetCounts();
				pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextSecondary.data(), false, "[Files] InUse: %u | Constructed: %u (%uKiB) | Memory Pool: %uKiB", pool.nUsed, pool.nAlloc, mem.nUsed / 1024, mem.nAlloc / 1024);
			}
			posX -= Debug::g_systemIndentation;

			if (g_pIImpl != nullptr)
			{
				Impl::SMemoryInfo memoryInfo;
				g_pIImpl->GetMemoryInfo(memoryInfo);

				posY += Debug::g_systemLineHeightClause;
				pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextPrimary.data(), false, "[Impl] Total Memory Used: %uKiB | Secondary Memory: %.2f / %.2f MiB | NumAllocs: %d",
				                      static_cast<uint32>(memoryInfo.totalMemory / 1024),
				                      (memoryInfo.secondaryPoolUsedSize / 1024) / 1024.0f,
				                      (memoryInfo.secondaryPoolSize / 1024) / 1024.0f,
				                      static_cast<int>(memoryInfo.secondaryPoolAllocations));

				posX += Debug::g_systemIndentation;
				posY += Debug::g_systemLineHeight;
				pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextSecondary.data(), false, "[Impl Object Pool] InUse: %u | Constructed: %u (%uKiB) | Memory Pool: %uKiB",
				                      memoryInfo.poolUsedObjects, memoryInfo.poolConstructedObjects, memoryInfo.poolUsedMemory, memoryInfo.poolAllocatedMemory);
				posX -= Debug::g_systemIndentation;
			}

			posY += Debug::g_systemLineHeightClause;

			static float const SMOOTHING_ALPHA = 0.2f;
			static float syncRays = 0;
			static float asyncRays = 0;
			Vec3 const& listenerPosition = g_listenerManager.GetActiveListenerTransformation().GetPosition();
			Vec3 const& listenerDirection = g_listenerManager.GetActiveListenerTransformation().GetForward();
			float const listenerVelocity = g_listenerManager.GetActiveListenerVelocity().GetLength();
			size_t const numObjects = g_objectManager.GetNumAudioObjects();
			size_t const numActiveObjects = g_objectManager.GetNumActiveAudioObjects();
			size_t const numEvents = g_eventManager.GetNumConstructed();
			size_t const numListeners = g_listenerManager.GetNumActiveListeners();
			size_t const numEventListeners = g_eventListenerManager.GetNumEventListeners();
			syncRays += (CPropagationProcessor::s_totalSyncPhysRays - syncRays) * SMOOTHING_ALPHA;
			asyncRays += (CPropagationProcessor::s_totalAsyncPhysRays - asyncRays) * SMOOTHING_ALPHA * 0.1f;

			bool const isActive = true;
			float const* colorListener = isActive ? Debug::g_systemColorListenerActive.data() : Debug::g_systemColorListenerInactive.data();

			if (numListeners > 0)
			{
				char const* const szName = g_listenerManager.GetActiveListenerName();
				pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, colorListener, false, "%s PosXYZ: %.2f %.2f %.2f FwdXYZ: %.2f %.2f %.2f Velocity: %.2f m/s", szName, listenerPosition.x, listenerPosition.y, listenerPosition.z, listenerDirection.x, listenerDirection.y, listenerDirection.z, listenerVelocity);
				posY += Debug::g_systemLineHeight;
			}

			pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorHeader.data(), false,
			                      "Objects: %3" PRISIZE_T "/%3" PRISIZE_T " Events: %3" PRISIZE_T " EventListeners %3" PRISIZE_T " Listeners: %" PRISIZE_T " | SyncRays: %3.1f AsyncRays: %3.1f",
			                      numActiveObjects, numObjects, numEvents, numEventListeners, numListeners, syncRays, asyncRays);

			posY += Debug::g_systemLineHeightClause;
		}

		string debugFilter = g_cvars.m_pDebugFilter->GetString();

		if (debugFilter.IsEmpty() || debugFilter == "0")
		{
			debugFilter = "<none>";
		}

		string debugDistance = ToString(g_cvars.m_debugDistance) + " m";

		if (g_cvars.m_debugDistance <= 0)
		{
			debugDistance = "<infinite>";
		}

		string debugDraw = "";

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::Spheres) != 0)
		{
			debugDraw += "Spheres, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectLabel) != 0)
		{
			debugDraw += "Labels, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectTriggers) != 0)
		{
			debugDraw += "Triggers, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectStates) != 0)
		{
			debugDraw += "States, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectParameters) != 0)
		{
			debugDraw += "Parameters, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectEnvironments) != 0)
		{
			debugDraw += "Environments, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectDistance) != 0)
		{
			debugDraw += "Distances, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::OcclusionRayLabels) != 0)
		{
			debugDraw += "Occlusion Ray Labels, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::OcclusionRays) != 0)
		{
			debugDraw += "Occlusion Rays, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ListenerOcclusionPlane) != 0)
		{
			debugDraw += "Listener Occlusion Plane, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ObjectStandaloneFiles) != 0)
		{
			debugDraw += "Object Standalone Files, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::StandaloneFiles) != 0)
		{
			debugDraw += "Standalone Files, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ActiveEvents) != 0)
		{
			debugDraw += "Active Events, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ActiveObjects) != 0)
		{
			debugDraw += "Active Objects, ";
		}

		if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::FileCacheManagerInfo) != 0)
		{
			debugDraw += "File Cache Manager, ";
		}

		if (!debugDraw.IsEmpty())
		{
			debugDraw.erase(debugDraw.length() - 2, 2);
			pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextPrimary.data(), false, "Debug Draw: %s", debugDraw.c_str());
			posY += Debug::g_systemLineHeight;
			pAuxGeom->Draw2dLabel(posX, posY, Debug::g_systemFontSize, Debug::g_systemColorTextPrimary.data(), false, "Debug Filter: %s | Debug Distance: %s", debugFilter.c_str(), debugDistance.c_str());

			posY += Debug::g_systemLineHeightClause;
		}

		DrawATLComponentDebugInfo(*pAuxGeom, posX, posY);
	}

	g_system.ScheduleIRenderAuxGeomForRendering(pAuxGeom);
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::GetAudioTriggerData(ControlId const audioTriggerId, STriggerData& audioTriggerData) const
{
	CTrigger const* const pTrigger = stl::find_in_map(g_triggers, audioTriggerId, nullptr);

	if (pTrigger != nullptr)
	{
		audioTriggerData.radius = pTrigger->GetRadius();
	}
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::DrawATLComponentDebugInfo(IRenderAuxGeom& auxGeom, float posX, float const posY)
{
	if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::FileCacheManagerInfo) != 0)
	{
		g_fileCacheManager.DrawDebugInfo(auxGeom, posX, posY);
		posX += 600.0f;
	}

	if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ActiveObjects) != 0)
	{
		g_objectManager.DrawDebugInfo(auxGeom, posX, posY);
		posX += 300.0f;
	}

	if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::ActiveEvents) != 0)
	{
		g_eventManager.DrawDebugInfo(auxGeom, posX, posY);
		posX += 600.0f;
	}

	if ((g_cvars.m_drawAudioDebug & Debug::EDrawFilter::StandaloneFiles) != 0)
	{
		g_fileManager.DrawDebugInfo(auxGeom, posX, posY);
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::RetriggerAudioControls()
{
	for (auto const pObject : g_objectManager.GetObjects())
	{
		pObject->ForceImplementationRefresh(true);
	}

	g_pObject->ForceImplementationRefresh(false);
}

///////////////////////////////////////////////////////////////////////////
void CAudioTranslationLayer::DrawAudioObjectDebugInfo(IRenderAuxGeom& auxGeom)
{
	g_objectManager.DrawPerObjectDebugInfo(auxGeom);
}
#endif // INCLUDE_AUDIO_PRODUCTION_CODE
}      // namespace CryAudio
