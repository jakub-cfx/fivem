#include <StdInc.h>

#include <CoreConsole.h>

#include <GameServer.h>
#include <HttpClient.h>

#include <ResourceEventComponent.h>
#include <ResourceManager.h>

#include <ServerInstanceBase.h>
#include <ServerInstanceBaseRef.h>

#include <ServerLicensingComponent.h>

#include <TcpListenManager.h>
#include <ReverseTcpServer.h>

#include <StructuredTrace.h>

#include <json.hpp>

using json = nlohmann::json;

struct ConditionalStartupNotice
{
public:
	ConditionalStartupNotice(const std::string& name, std::function<bool()> conditionsFunc, std::function<void()> actionsFunc)
		: m_name(name), m_conditionsFunc(std::move(conditionsFunc)), m_actionsFunc(std::move(actionsFunc)){};

	void ProcessNotice()
	{
		auto conditionsMet = m_conditionsFunc();
		if (conditionsMet)
		{
			trace("^1-- [server notice: %s]^7\n", m_name);
			m_actionsFunc();
		}
	}

private:
	std::string m_name;
	std::function<bool()> m_conditionsFunc;
	std::function<void()> m_actionsFunc;
};

static void SetupAndProcessNotices(fx::ServerInstanceBase* server)
{
	// Get ConVar manager once
	auto cvMan = server->GetComponent<console::Context>()->GetVariableManager();

	// Configure notice structs
	ConditionalStartupNotice notices[] = {
		ConditionalStartupNotice(
		std::string("hostname_rework"),
		[&cvMan]()
		{
			auto svProjectName = cvMan->FindEntryRaw("sv_projectName");
			return !svProjectName || svProjectName->GetValue() == "";
		},
		[]()
		{
			trace("^2You don't have sv_projectName/sv_projectDesc set.\n^2These variables augment sv_hostname and fix your server name being cut off in the server list.^7\nUse `sets sv_projectName ..` and `sets sv_projectDesc ..` to set them.\n");
		}),

		// Commented out to align with `and false` condition in source JSON at time of writing & not waste time processing a notice that will always eval to FALSE
		//ConditionalStartupNotice(
		//std::string("tebex_not_set"),
		//[&cvMan]()
		//{
		//	auto svTebexSecret = cvMan->FindEntryRaw("sv_tebexSecret");
		//	return !svTebexSecret || svTebexSecret->GetValue() == "";
		//},
		//[]()
		//{
		//	trace("^1================^7\nMonetize your server using Tebex! Visit ^2https://tebex.io/fivem^7 for more info.\n^1================^7\n");
		//}),

		ConditionalStartupNotice(
		std::string("stay_safe"),
		[&cvMan]()
		{
			auto version = cvMan->FindEntryRaw("version");
			return version->GetValue().find("no-version") != std::string::npos;
		},
		[]()
		{
			trace("^2Note: You are using an unsupported custom server build. Please take care.^7\n");
		})
	};

	// Process structs
	for (auto& n : notices)
	{
		n.ProcessNotice();
	}
}


static InitFunction initFunction([]()
{
	static auto httpClient = new HttpClient();

	static ConsoleCommand printCmd("print", [](const std::string& str)
	{
		trace("%s\n", str);
	});

	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		using namespace std::chrono_literals;

		static bool setNucleus = false;
		static bool setNucleusSuccess = false;
		static std::chrono::milliseconds setNucleusTimeout;

		instance->GetComponent<fx::GameServer>()->OnTick.Connect([instance]()
		{
			if (!setNucleusSuccess && (!setNucleus || (msec() > setNucleusTimeout)))
			{
				auto var = instance->GetComponent<console::Context>()->GetVariableManager()->FindEntryRaw("sv_licenseKeyToken");

				if (var && !var->GetValue().empty())
				{
					auto licensingComponent = instance->GetComponent<ServerLicensingComponent>();
					auto nucleusToken = licensingComponent->GetNucleusToken();

					if (!nucleusToken.empty())
					{
						auto tlm = instance->GetComponent<fx::TcpListenManager>();

						auto jsonData = nlohmann::json::object({
							{ "token", nucleusToken },
							{ "port", fmt::sprintf("%d", tlm->GetPrimaryPort()) },
							{ "ipOverride", instance->GetComponent<fx::GameServer>()->GetIpOverrideVar()->GetValue() },
						});

						static auto authDelay = 15s;

						setNucleusTimeout = msec() + authDelay;

						HttpRequestOptions opts;
						opts.ipv4 = true;

						httpClient->DoPostRequest("https://cfx.re/api/register/?v=2", jsonData.dump(), opts, [instance, tlm](bool success, const char* data, size_t length)
						{
							if (!success)
							{
								if (authDelay < 15min)
								{
									authDelay *= 2;
								}

								setNucleusTimeout = msec() + authDelay;
							}
							else
							{
								auto jsonData = nlohmann::json::parse(std::string(data, length));

								trace("^1        fff                          \n^1  cccc ff   xx  xx     rr rr    eee  \n^1cc     ffff   xx       rrr  r ee   e \n^1cc     ff     xx   ... rr     eeeee  \n^1 ccccc ff   xx  xx ... rr      eeeee \n                                     ^7\n");
								trace("^2Authenticated with cfx.re Nucleus: ^7https://%s/\n", jsonData.value("host", ""));

								fwRefContainer<net::ReverseTcpServer> rts = new net::ReverseTcpServer();
								rts->Listen("users.cfx.re:30130", jsonData.value("rpToken", ""));

								tlm->AddExternalServer(rts);

								instance->GetComponent<fx::ResourceManager>()
									->GetComponent<fx::ResourceEventManagerComponent>()
									->QueueEvent2(
										"_cfx_internal:nucleusConnected",
										{},
										fmt::sprintf("https://%s/", jsonData.value("host", ""))
									);

								StructuredTrace({ "type", "nucleus_connected" }, { "url", fmt::sprintf("https://%s/", jsonData.value("host", "")) });

								static auto webVar = instance->AddVariable<std::string>("web_baseUrl", ConVar_None, jsonData.value("host", ""));

								setNucleusSuccess = true;
							}

							SetupAndProcessNotices(instance);
						});
					}

					setNucleus = true;
				}
			}
		});
	}, INT32_MAX);
});
