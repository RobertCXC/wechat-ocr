﻿#include "stdafx.h"
#include "mojocall.h"
#include "mmmojo.h"

#ifdef _DEBUG
#define DBG_PRINT(fmt, ...) fprintf(stderr, "[DEBUG]" fmt, __VA_ARGS__)
#else
#define DBG_PRINT(fmt, ...)
#endif

CMojoCall::~CMojoCall()
{
	Stop();
	if (m_mod)
	{
		FreeLibrary(m_mod);
		m_mod = 0;
	}
}

bool CMojoCall::Init(LPCWSTR wcdir)
{
	if (!m_mod) {
		// find mmmojo(_64).dll
		std::wstring mojo_dll = wcdir;
		if (mojo_dll.empty()) return false;
		if (mojo_dll.back() != L'\\') mojo_dll += L'\\';
		size_t osz = mojo_dll.size();
#ifdef _WIN64
		mojo_dll += L"mmmojo_64.dll";
#else
		mojo_dll += L"mmmojo.dll";
#endif
		auto mod = GetModuleHandleW(mojo_dll.c_str() + osz);
		if (!mod) mod = LoadLibraryW(mojo_dll.c_str());
		if (mod == NULL) return false;
		m_mod = mod;
		InitializeMMMojo(0, 0);
	}

	return true;
}

bool CMojoCall::Start(LPCWSTR exepath)
{
	if (!m_env)
	{
		//创建MMMojo环境
		auto env = CreateMMMojoEnvironment();
		if (!env) return false;

		//设置回调函数
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMUserData, this);
		void (*ReadOnPush)(uint32_t request_id, const void* request_info, void* user_data) = [](uint32_t request_id, const void* request_info, void* user_data) {
			DBG_PRINT("ReadOnPush: %u\n", request_id);
			return ((CMojoCall*)user_data)->ReadOnPush(request_id, request_info);
		};
		void (*ReadOnPull)(uint32_t request_id, const void* request_info, void* user_data) = [](uint32_t request_id, const void* request_info, void* user_data) {
			DBG_PRINT("ReadOnPull: %u\n", request_id);
			return ((CMojoCall*)user_data)->ReadOnPull(request_id, request_info);
		};
		void (*ReadOnShared)(uint32_t request_id, const void* request_info, void* user_data) = [](uint32_t request_id, const void* request_info, void* user_data) {
			DBG_PRINT("ReadOnShared: %u\n", request_id);
			return ((CMojoCall*)user_data)->ReadOnShared(request_id, request_info);
		};

		void (*OnConnect)(bool is_connected, void* user_data) = [](bool is_connected, void* user_data) {
			DBG_PRINT("OnConnect: %d\n", is_connected);
			return ((CMojoCall*)user_data)->OnRemoteConnect(is_connected);
		};
		void (*OnDisConnect)(void* user_data) = [](void* user_data) {
			DBG_PRINT("OnDisConnect\n");
			return ((CMojoCall*)user_data)->OnRemoteDisConnect();
		};
		void (*OnProcessLaunched)(void* user_data) = [](void* user_data) {
			DBG_PRINT("OnProcessLaunched\n");
			return ((CMojoCall*)user_data)->OnRemoteProcessLaunched();
		};
		void (*OnProcessLaunchFailed)(int error_code, void* user_data) = [](int error_code, void* user_data) {
			DBG_PRINT("OnProcessLaunchFailed: %d\n", error_code);
			return ((CMojoCall*)user_data)->OnRemoteProcessLaunchFailed(error_code);
		};
		void (*OnError)(const void* errorbuf, int errorsize, void* user_data) = [](const void* errorbuf, int errorsize, void* user_data) {
			DBG_PRINT("OnError: %.*s\n", errorsize, (const char*)errorbuf);
			return ((CMojoCall*)user_data)->OnRemoteError(errorbuf, errorsize);
		};

		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMReadPush, ReadOnPush);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMReadPull, ReadOnPull);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMReadShared, ReadOnShared);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMRemoteConnect, OnConnect);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMRemoteDisconnect, OnDisConnect);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMRemoteProcessLaunched, OnProcessLaunched);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMRemoteProcessLaunchFailed, OnProcessLaunchFailed);
		SetMMMojoEnvironmentCallbacks(env, MMMojoEnvironmentCallbackType::kMMRemoteMojoError, OnError);
		m_env = env;
	}

	//设置启动所需参数
	SetMMMojoEnvironmentInitParams(m_env, MMMojoEnvironmentInitParamType::kMMHostProcess, TRUE);
	SetMMMojoEnvironmentInitParams(m_env, MMMojoEnvironmentInitParamType::kMMExePath, exepath);

	//设置SwitchNative命令行参数
	for (const auto& [k, v] : m_args)
	{
		AppendMMSubProcessSwitchNative(m_env, k.c_str(), v.c_str());
	}
	StartMMMojoEnvironment(m_env);
	return true;
}

void CMojoCall::Stop()
{
	if (m_env)
	{
		StopMMMojoEnvironment(m_env);
		RemoveMMMojoEnvironment(m_env);
		m_env = 0;
	}
}

bool CMojoCall::SendPbSerializedData(const void* pb_data, size_t data_size, int method, bool sync, uint32_t request_id)
{
	if (data_size < 0) return false;

	void* write_info = CreateMMMojoWriteInfo(method, sync, request_id);
	if (data_size != 0)
	{
		void* request = GetMMMojoWriteInfoRequest(write_info, data_size);
		// TODO: 这里是否有内存泄漏? request，write_info指针会释放吗?
		memcpy(request, pb_data, data_size);//写入protobuf数据
		return SendMMMojoWriteInfo(m_env, write_info);
	}
	RemoveMMMojoWriteInfo(write_info);
	return false;
}

void CMojoCall::OnRemoteConnect(bool is_connected)
{
	std::lock_guard<std::mutex> lock(m_mutex_state);
	m_state = is_connected ? MJC_CONNECTED : MJC_FAILED;
	m_cv_state.notify_all();
}

void CMojoCall::OnRemoteDisConnect() {
	std::lock_guard<std::mutex> lock(m_mutex_state);
	m_state = MJC_FAILED;
	m_cv_state.notify_all();
}

bool CMojoCall::wait_connection(int timeout)
{
	if (timeout < 0) {
		std::unique_lock<std::mutex> lock(m_mutex_state);
		m_cv_state.wait(lock, [this] {return m_state != MJC_PENDING; });
	}
	else
	{
		std::unique_lock<std::mutex> lock(m_mutex_state);
		if (!m_cv_state.wait_for(lock, std::chrono::milliseconds(timeout), [this] {return m_state != MJC_PENDING; }))
		{
			return false;
		}
	}
	return m_state >= MJC_CONNECTED;
}

void CMojoCall::OnRemoteProcessLaunchFailed(int error_code)
{
	std::lock_guard<std::mutex> lock(m_mutex_state);
	if (m_state == MJC_PENDING) {
		m_state = MJC_FAILED;
		m_cv_state.notify_all();
	}
}