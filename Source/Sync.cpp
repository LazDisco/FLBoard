#pragma warning (disable: 4503)

#include <sstream>

#include "Sync.h"
#include "Misc.h"
#include "Thread.h"
#include "Common.h"

using namespace Misc;
using namespace raincious::FLHookPlugin::Board;

namespace raincious
{
	namespace FLHookPlugin
	{
		namespace Board
		{
			namespace Sync
			{
				// Client
				Client::Instances Client::instances;
				
				CRITICAL_SECTION Client::staticQueueLock;
				bool Client::inited = false;
				bool Client::releasing = false;

				APIServer Client::Get(APILogin clientLogin)
				{
					APIServer serverInfo;

					if (!inited)
					{
						inited = true;

						Http::Http::StartUp();

						InitializeCriticalSection(&staticQueueLock);
					}

					releasing = false;

					EnterCriticalSection(&staticQueueLock);

					Client* client = new Client(clientLogin, serverInfo);

					instances.push_back(client);

					LeaveCriticalSection(&staticQueueLock);

					return serverInfo;
				}

				void Client::Send(DataItem data)
				{
					bool addFailed = false;
					Instances::iterator it;

					if (!inited)
					{
						return;
					}

					if (releasing)
					{
						return;
					}

					for (it = instances.begin(); it != instances.end(); it++)
					{
						if (!(*it)->addQueue(data))
						{
							addFailed = true;
						}
					}

					// Do a dobule check, the value of releasing may changed during time.
					if (!addFailed && !releasing)
					{
						Thread::Worker::Activate();
					}
				}

				void Client::Release()
				{
					Instances::iterator it;

					if (!inited)
					{
						// Release my axx, you don't even inited.
						return;
					}

					releasing = true;

					EnterCriticalSection(&staticQueueLock);

					if (!instances.empty())
					{
						while (instances.size() > 0)
						{
							Client* instance = instances.back();
							instances.pop_back();

							delete instance;
						}
					}

					Http::Http::CleanUp();

					inited = false;

					LeaveCriticalSection(&staticQueueLock);

					DeleteCriticalSection(&staticQueueLock);
				}

				bool Client::Run(APIResponsePackages* packages)
				{
					if (!inited)
					{
						return true;
					}

					Instances::iterator instanceIter;
					bool skip = false;
					bool result = true;

					for (instanceIter = instances.begin(); instanceIter != instances.end(); instanceIter++)
					{
						APIResponses responses;
						APIResponsePackage package;
						APIResponseStatus status = (*instanceIter)->sendQueue(&responses);

						switch (status)
						{
						case PERIOD_LIMIT:
							result = false; // False for retry
							break;

						default:
							break;
						}

						if (responses.size() > 0)
						{
							package.API = (*instanceIter)->server.Name;
							package.Responses = responses;

							(*packages).push_back(package);
						}
					}

					return result;
				}

				Client::Client(APILogin client, APIServer &serverInfo)
				{
					wstring message;
					string loginErrorMessage;
					APIResponseStatus loginResponse;
					InitializeCriticalSection(&queueSycLock);

					server.Token = "";
					server.Delay = 0;
					server.Name = L"";
					server.QueueLimit = 0;
					server.LastSent = 0;

					enabled = false;
					skips = 0;

					loginInfo = client;
					
					verifyer.key(loginInfo.Secret);

					loginResponse = login(server, loginErrorMessage);

					if (loginResponse != SUCCEED)
					{
						message = L"API Login failed: ";

						switch (loginResponse) {
						case FAILED_STATUS:
							message.append(L"Server error");
							break;

						case INVALID_RESPONSE:
							message.append(L"Server response invalid");
							break;

						case INVALID_TOKEN:
							message.append(L"Server responses invalid token");
							break;

						case INVALID_SECERT:
							message.append(L"Server unidentified");
							break;

						case INVALID_ACK:
							message.append(L"Server failed to acknowledge");
							break;

						case ERROR_MESSAGE:
							message.append(L"Server response a error message: ");
							message.append(wstring(loginErrorMessage.begin(), loginErrorMessage.end()));
							break;
						}

						message.append(L". Player information will not send to server: ");
						message.append(wstring(loginInfo.URI.begin(), loginInfo.URI.end()));
						message.append(L".");

						Common::PrintConInfo(message, true);
					}
					else
					{
						serverInfo = server;
						loggedIn = true;
						enabled = true;

						// Want to read token don't you?
						serverInfo.Token = "";
					}
				}

				Client::~Client()
				{
					logoff();

					enabled = false; // Disable sync

					EnterCriticalSection(&queueSycLock);

					Queue e;
					swap(sendingQueue, e);

					LeaveCriticalSection(&queueSycLock);

					DeleteCriticalSection(&queueSycLock);
				}

				void Client::printError(wstring error)
				{
					if (!Common::DebugPrintEnabled())
					{
						return;
					}

					wstring message = L"[Synchronizing " + (server.Name) + L"] " + error + L" <";

					message.append(L"Delay: ");
					message.append(Encode::stringToWstring(itos(server.Delay)));

					message.append(L", Queue: ");
					message.append(Encode::stringToWstring(itos(server.QueueLimit)));

					message.append(L", Addr: ");
					message.append(wstring(loginInfo.URI.begin(), loginInfo.URI.end()));

					message.append(L">");

					Common::PrintConInfo(message);
				}

				void Client::setJsonCommonHeader(Http::HttpHandler& http)
				{
					http.header("Accept: application/json");
					http.header("Content-Type: application/json");
					http.header("charsets: utf-8");
				}

				APIResponseStatus Client::login(APIServer &serverInfo, string &errorMessage)
				{
					Json::Value loginParameter;
					Json::Value responseRoot;
					Json::Value responseToken;
					Json::Reader responseReader;

					loginParameter["Task"] = "Login";
					loginParameter["Cecret"] = verifyer.gen();
					loginParameter["Account"] = Encode::UTF8Encode(loginInfo.Account);
					loginParameter["Password"] = Encode::UTF8Encode(loginInfo.Password);
					loginParameter["Queue"] = loginInfo.Queue;

					Http::Post http(loginInfo.URI);

					setJsonCommonHeader(http);

					http.data(loginParameter.toStyledString(), "");

					string result = http.result();

					// Server not return the right status
					if (http.status() != 200)
					{
						printError(L"Bad HTTP status on Login");

						return FAILED_STATUS;
					}

					// Server returns invalid content format
					if (!responseReader.parse(result, responseRoot))
					{
						printError(L"Invalid response on login");

						return INVALID_RESPONSE;
					}

					if (!verifyer.pair(loginParameter["Cecret"].asInt(), JsonHelper::GetValueInt(responseRoot, "Cecret")))
					{
						printError(L"Invalid secret response on login");

						return INVALID_SECERT;
					}

					if (JsonHelper::GetValueStr(responseRoot, "Ack") != loginParameter["Task"].asString())
					{
						printError(L"Invalid acknowledge response on login");

						return INVALID_ACK;
					}

					// Get server error information

					errorMessage = JsonHelper::GetValueStr(responseRoot, "Error");
					if (errorMessage != "")
					{
						printError(L"Server error:" + wstring(errorMessage.begin(), errorMessage.end()));

						return ERROR_MESSAGE;
					}

					serverInfo.Token = JsonHelper::GetValueStr(responseRoot, "Token");

					// Actuall, use a string node on all integer and numbers, bools
					serverInfo.Delay = JsonHelper::GetValueUint(
						responseRoot, "Delay") * 1000; // Server can only return seconds

					serverInfo.Name = Encode::UTF8Decode(
						JsonHelper::GetValueStr(responseRoot, "Name")
						);

					serverInfo.QueueLimit = JsonHelper::GetValueUint(
						responseRoot, "Queue");

					serverInfo.LastSent = clock();

					// API Server not returning valid token
					if (server.Token == "")
					{
						printError(L"Returning invalid token");

						return INVALID_TOKEN;
					}

					// API Server friendly name
					if (server.Name == L"")
					{
						server.Name = L"Untitled ";
						server.Name.append(wstring(loginInfo.URI.begin(), loginInfo.URI.end()));
					}

					// API Server delay time
					if (server.Delay <= 0)
					{
						printError(L"Delay " + Encode::stringToWstring(itos(server.Delay)) + L" is invalid, using default");

						server.Delay = SYNC_CLIENT_MAX_DELAY;
					}
					else if (server.Delay > SYNC_CLIENT_MAX_DELAY)
					{
						printError(L"Delay " + Encode::stringToWstring(itos(server.Delay)) + L" greater than limit, using limit as maximum");

						server.Delay = SYNC_CLIENT_MAX_DELAY;
					}

					// API Server delay time
					if (server.QueueLimit <= 0)
					{
						printError(L"Queue " + Encode::stringToWstring(itos(server.QueueLimit)) + L" is invalid, using default");

						server.QueueLimit = SYNC_CLIENT_DEFAULT_QUEUE;
					}
					else if (loginInfo.Queue > 0 && server.QueueLimit > loginInfo.Queue)
					{
						printError(L"Queue " + Encode::stringToWstring(itos(server.QueueLimit)) + L" greater than limit, using " + Encode::stringToWstring(itos(loginInfo.Queue)) + L" as maximum");

						server.QueueLimit = loginInfo.Queue;
					}
					else if (server.QueueLimit > SYNC_CLIENT_MAX_QUEUE)
					{
						printError(L"Queue " + Encode::stringToWstring(itos(server.QueueLimit)) + L" greater than limit, using maximum as limit");

						server.QueueLimit = SYNC_CLIENT_MAX_QUEUE;
					}

					printError(L"Logged in");

					return SUCCEED;
				}

				APIResponseStatus Client::logoff()
				{
					Json::Value logoffParameter;
					Json::Value responseParameter;
					Json::Reader responseReader;

					Http::Post http(loginInfo.URI);

					setJsonCommonHeader(http);

					if (!loggedIn) 
					{
						return FAILED_LOGIN;
					}

					logoffParameter["Task"] = "Logoff";
					logoffParameter["Cecret"] = verifyer.gen();
					logoffParameter["Token"] = server.Token;

					http.data(logoffParameter.toStyledString(), "");

					string result = http.result();

					if (http.status() != 200)
					{
						printError(L"Bad HTTP status on logout");

						return FAILED_STATUS;
					}

					if (!responseReader.parse(result, responseParameter))
					{
						printError(L"Bad response on logout");

						return INVALID_RESPONSE;
					}

					if (!verifyer.pair(logoffParameter["Cecret"].asInt(), JsonHelper::GetValueInt(responseParameter, "Cecret")))
					{
						printError(L"Bad secret response logout");

						return INVALID_SECERT;
					}

					if (JsonHelper::GetValueStr(responseParameter, "Ack") != logoffParameter["Task"].asString())
					{
						printError(L"Bad acknowledge response logout");

						return INVALID_ACK;
					}

					printError(L"Logged out");

					return SUCCEED;
				}

				bool Client::isTaskAllowsResponse(string taskType)
				{
					// Allows when not set any
					if (loginInfo.Responses.empty()) {
						return true;
					}

					// So it has set, allow it
					if (find(loginInfo.Responses.begin(), loginInfo.Responses.end(), taskType) != loginInfo.Responses.end())
					{
						return true;
					}

					return false;
				}

				bool Client::isAllowedSendOperate(string reqeustType)
				{
					// Allows when not set any
					if (loginInfo.Operations.empty()) {
						return true;
					}

					// So it has set, allow it
					if (find(loginInfo.Operations.begin(), loginInfo.Operations.end(), reqeustType) != loginInfo.Operations.end())
					{
						return true;
					}

					return false;
				}

				APIResponseStatus Client::sync(Json::Value root, APIResponses* responses, bool noRetry)
				{
					Json::Value sendParameter;
					Json::Value responseRoot;
					Json::Value responseToken;
					Json::Reader responseReader;
					string errMessage = "";

					// Check if server is available
					if (!enabled)
					{
						printError(L"Disabled, no synchronizing. Data dropped.");

						return DISABLED;
					}

					// Check skips
					if (skips > 0)
					{
						skips--;

						printError(L"Skipped, no synchronizing. Data dropped.");

						return SKIPED;
					}

					sendParameter["Task"] = "Sync";
					sendParameter["Token"] = server.Token;
					sendParameter["Cecret"] = verifyer.gen();
					sendParameter["Datas"] = root;

					Http::Post http(loginInfo.URI);

					setJsonCommonHeader(http);

					http.data(sendParameter.toStyledString(), "");

					string result = http.result();
					
					// Catch status error and try handle it when we can
					switch (http.status())
					{
					case 200:
						// Level a break to pass, every case below shall return
						break;

					case 403:
						if (noRetry)
						{
							printError(L"Synchronizing, but token not working any more. Tried relogin but failed. Dropped.");

							return FAILED_LOGIN;
						}

						if (login(server, errMessage) == SUCCEED)
						{
							return sync(root, responses, true);
						}
						else
						{
							skips += 20;
						}
						break;

					default:
						skips += 5;

						printError(L"Invalid HTTP status on synchronizing");

						return FAILED_STATUS;
					}

					// Parse response
					if (!responseReader.parse(result, responseRoot))
					{
						printError(L"Invalid response on synchronizing");

						return INVALID_RESPONSE;
					}

					if (!responseRoot.isObject())
					{
						printError(L"Invalid response on synchronizing");

						return INVALID_RESPONSE;
					}

					if (!verifyer.pair(sendParameter["Cecret"].asInt(), JsonHelper::GetValueInt(responseRoot, "Cecret")))
					{
						printError(L"Invalid secret response on synchronizing");

						return INVALID_SECERT;
					}

					if (JsonHelper::GetValueStr(responseRoot, "Ack") != sendParameter["Task"].asString())
					{
						printError(L"Invalid acknowledge response on synchronizing");

						return INVALID_ACK;
					}

					if (!responseRoot["Tasks"].isArray())
					{
						skips += 10;

						printError(L"Invalid task data on synchronizing");

						return INVALID_DATA;
					}

					size_t taskSize = responseRoot["Tasks"].size();

					// We only fetch this number tops
					// Notice this number is limited by SYNC_CLIENT_MAX_QUEUE when reading server info
					if (taskSize > server.QueueLimit)
					{
						taskSize = server.QueueLimit;
					}

					for (size_t taskLoop = 0; taskLoop < taskSize; taskLoop++)
					{
						APIResponseTask responseTask;
						string responseType;
						Json::Value responseData;

						if (!responseRoot["Tasks"][taskLoop].isObject())
						{
							printError(L"Invalid task data item on synchronizing. Skipped.");

							continue;
						}

						responseType = JsonHelper::GetValueStr(responseRoot["Tasks"][taskLoop], "Type");

						if (responseType == "")
						{
							printError(L"Response task type not set.");
							continue;
						}

						if (!isTaskAllowsResponse(responseType))
						{
							printError(L"Unallowed Response task type " + wstring(responseType.begin(), responseType.end()) + L". Skipped.");

							continue;
						}

						responseData = responseRoot["Tasks"][taskLoop]["Data"];

						if (!responseData.isObject())
						{
							printError(L"Invalid response task data. Skipped.");

							continue;
						}

						vector <string> dataKeys = responseData.getMemberNames();
						vector <string>::iterator dataIter;

						for (dataIter = dataKeys.begin(); dataIter != dataKeys.end(); dataIter++)
						{
							string dataKey = dataIter->c_str();

							responseTask.Data[Encode::UTF8Decode(dataKey)] =
								Encode::UTF8Decode(JsonHelper::GetValueStr(responseData, dataIter->c_str()));
						}

						responseTask.Type = responseType;

						(*responses).push_back(responseTask);
					}

					return SUCCEED;
				}

				bool Client::addQueue(DataItem data)
				{
					uint qLength = 0;

					if (!enabled)
					{
						printError(L"Disabled, can't add queue to this API.");

						return false;
					}
					
					EnterCriticalSection(&queueSycLock);

					qLength = sendingQueue.size();

					if (qLength > server.QueueLimit)
					{
						printError(L"Queue over API server limit. Dropping old.");

						sendingQueue.pop();
					}

					sendingQueue.push(data);

					LeaveCriticalSection(&queueSycLock);

					return true;
				}

				APIResponseStatus Client::sendQueue(APIResponses* responses)
				{
					Json::Value items = Json::Value(Json::arrayValue);
					DataItem queueItem;
					clock_t currentTime = clock();
					Queue opearatingQueue, emptyQueue;
					uint addedItems = 0;
					
					if (!enabled)
					{
						printError(L"Disabled, can't send queue to this API server.");

						return DISABLED;
					}

					if (currentTime < (clock_t)(server.LastSent + server.Delay))
					{
						return PERIOD_LIMIT;
					}
					
					// OK, let me copy this queue so we will get rid of lock problem
					EnterCriticalSection(&queueSycLock);

					server.LastSent = currentTime;

					if (!sendingQueue.empty())
					{
						swap(opearatingQueue, sendingQueue);
						// Now the empty opearatingQueue becomes new sendingQueue
						// And the sendingQueue becomes new opearatingQueue with datas
					}

					// Unlock, the queue are free
					LeaveCriticalSection(&queueSycLock);
					
					// Loop all queues until it empty
					while (!opearatingQueue.empty())
					{
						queueItem = opearatingQueue.front();

						DataItem::iterator itemIterator;
						DataValue::iterator valueIterator;
						
						Json::Value item;

						for (itemIterator = queueItem.begin(); itemIterator != queueItem.end(); itemIterator++)
						{
							if (!isAllowedSendOperate(itemIterator->first)) {
								continue;
							}

							Json::Value value;
							for (valueIterator = itemIterator->second.begin(); valueIterator != itemIterator->second.end(); valueIterator++)
							{
								value[Encode::UTF8Encode(valueIterator->first)] =
									Encode::UTF8Encode(valueIterator->second);
							}

							item["Type"] = Encode::UTF8Encode(itemIterator->first);
							item["Data"] = value;

							addedItems++;
						}

						items.append(item);

						opearatingQueue.pop();
					}

					if (items.empty() || addedItems <= 0)
					{
						printError(L"No task need to send to this API. Skipped.");

						return NO_TASK;
					}
					
					return sync(items, responses, false);
				}

				// Listener
				Listener::Events Listener::events;
				Listener::CallingLock Listener::callingLock;
				Listener::CallableCallback Listener::callable;

				void Listener::Listen(string eventName, EventCallback eventCallback)
				{
					InitializeCriticalSection(&callingLock[eventCallback]);

					EnterCriticalSection(&callingLock[eventCallback]);

					events[eventName].Handlers.push_back(eventCallback);

					callable.insert(eventCallback);

					LeaveCriticalSection(&callingLock[eventCallback]);

					printError(L"Callback added for " + wstring(eventName.begin(), eventName.end()));
				}

				void Listener::Unlisten(string eventName, EventCallback eventCallback)
				{
					EnterCriticalSection(&callingLock[eventCallback]);

					CallableCallback::iterator iter = callable.find(eventCallback);

					if (iter != callable.end())
					{
						callable.erase(iter);
					}

					events[eventName].Handlers.remove(eventCallback);

					LeaveCriticalSection(&callingLock[eventCallback]);

					DeleteCriticalSection(&callingLock[eventCallback]);

					printError(L"Callback released for " + wstring(eventName.begin(), eventName.end()));
				}

				void Listener::Run(Sync::APIResponsePackages &packages, bool &noDelay)
				{
					uint listenerCalls = 0;
					double totalTime = 0, groupRunTime = 0;
					APIResponses::iterator responsesIter;
					APIResponsePackages::iterator packageIter;
					wstringstream wss;

					printError(L"Calling response handlers");

					for (packageIter = packages.begin(); packageIter != packages.end(); packageIter++)
					{
						printError(L"Handling response task for API " + (*packageIter).API);

						for (responsesIter = (*packageIter).Responses.begin(); responsesIter != (*packageIter).Responses.end(); responsesIter++)
						{
							Data::Parameter parameter(responsesIter->Data);

							trigger((*packageIter).API, responsesIter->Type, parameter, groupRunTime, noDelay);

							totalTime += groupRunTime;

							listenerCalls++;
						}

						printError(L"Finished");
					}

					wss << L"All ";
					wss << listenerCalls;
					wss << L" handlers finished in ";
					wss << ((totalTime) / CLOCKS_PER_SEC) * 1000;
					wss << L"ms";

					printError(wss.str());

					return;
				}

				void Listener::trigger(wstring source, string eventName, Data::Parameter response, double &totalTime, bool &noDelay)
				{
					double startTime = 0, finishTime = 0; // Can't use clock_t, or you get X000ms

					Events::iterator eventIter = events.find(eventName);

					if (eventIter == events.end()) {
						return;
					}

					EventHandlers::iterator eHandlerIter;
					for (eHandlerIter = events[eventName].Handlers.begin(); eHandlerIter != events[eventName].Handlers.end(); eHandlerIter++)
					{
						EnterCriticalSection(&callingLock[*eHandlerIter]);

						printError(L"+ Firing for " + wstring(eventName.begin(), eventName.end()));

						startTime = clock();

						if (callable.find(*eHandlerIter) != callable.end())
						{
							(*eHandlerIter)(source, response);

							printError(L"- Fired");
						}
						else
						{
							printError(L"- ! Unfireable");
						}

						finishTime = clock();

						totalTime = finishTime - startTime;

						LeaveCriticalSection(&callingLock[*eHandlerIter]);

						if (!noDelay)
						{
							Sleep(100);
						}
					}
				}

				void Listener::printError(wstring error)
				{
					if (!Common::DebugPrintEnabled())
					{
						return;
					}

					wstring message = L"[Responsing] " + error + L" <";

					message.append(L"Events: ");
					message.append(Encode::stringToWstring(itos(events.size())));

					message.append(L">");

					Common::PrintConInfo(message);
				}
			}
		}
	}
}
