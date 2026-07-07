using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Abp.Domain.Repositories;
using Abp.Domain.Services;
using Abp.UI;
using Akka.Actor;
using HashidsNet;
using Newtonsoft.Json;
using WssdEngine.Actors;
using WssdEngine.Actors.MessageModels;
using WssdEngine.DomainServices.FlowControlEngine;
using WssdEngine.DomainServices.Nalo.Dto;
using WssdEngine.DomainServices.PosService;
using WssdEngine.DomainServices.PosService.Dto;
using WssdEngine.Models;
using WssdEngine.Redis;
using WssdEngine.Utils;

namespace WssdEngine.DomainServices.Nalo;

public class NaloUssdInteractionManager: DomainService, INaloUssdInteractionManager
{
    private readonly IRedisCacheManager _redisCacheManager;
    private readonly IRepository<WssdResource, Guid> _repositoryWssdResource;
    private readonly IActorRef _wssdSessionStorageActorProvider;
    private readonly IActorRef _deferredRoutineActorProvider;
    private readonly IActorRef _sessionBalanceActorProvider;
    private readonly IActorRef _flexConnectInvocationActor;
    private readonly IPosApi _posApi;
    private readonly IHashids _hashids;
    private const int DefaultPageSize = 5; // Default number of menu items per page
    private const string NextPageOption = "98. Next Page";
    private const string PrevPageOption = "99. Previous Page";
     
    private readonly ConcurrentDictionary<string, WssdResourceInteractionConfig> _configCache = new();
    
    // Cache key format constants
    private const string SESSION_NAVIGATION_STATE_KEY_FORMAT = "ussd-navigation-state:{0}{1}";
    private const string DATA_DICTIONARY_KEY_FORMAT = "input:{0}{1}:data";
    private const string RETRY_COUNT_KEY_FORMAT = "retry:{0}:{1}";
    private const string MENU_CACHE_KEY_FORMAT = "nav:{0}{1}:state";
    private const string INPUT_CACHE_KEY_FORMAT = "input:{0}{1}:state";

    // Helper method to format cache keys
    private string GetNavigationStateKey(string sessionId, Guid resourceId) => 
        string.Format(SESSION_NAVIGATION_STATE_KEY_FORMAT, sessionId, resourceId);
    
    private string GetDataDictionaryKey(string sessionId, Guid resourceId) => 
        string.Format(DATA_DICTIONARY_KEY_FORMAT, sessionId, resourceId);
    
    private string GetRetryKey(string sessionId, Guid menuOptionId) => 
        string.Format(RETRY_COUNT_KEY_FORMAT, sessionId, menuOptionId);
    
    private string GetMenuCacheKey(string sessionId, Guid resourceId) => 
        string.Format(MENU_CACHE_KEY_FORMAT, sessionId, resourceId);
    
    private string GetInputCacheKey(string sessionId, Guid resourceId) => 
        string.Format(INPUT_CACHE_KEY_FORMAT, sessionId, resourceId);
    
    
    public NaloUssdInteractionManager(IRedisCacheManager redisCacheManager, TopLevelActors.WssdSessionStorageActorProvider wssdSessionStorageActorProvider, TopLevelActors.FlexConnectInvocationActorProvider flexConnectInvocationActorProvider, TopLevelActors.DeferredRoutineActorProvider deferredRoutineActorProvider, TopLevelActors.SessionBalanceActorProvider sessionBalanceActorProvider, IRepository<WssdResource, Guid> repositoryWssdResource, IPosApi posApi, IHashids hashids)
    {
        _redisCacheManager = redisCacheManager;
        _repositoryWssdResource = repositoryWssdResource;
        _posApi = posApi;
        _hashids = hashids;
        _wssdSessionStorageActorProvider = wssdSessionStorageActorProvider();
        _sessionBalanceActorProvider = sessionBalanceActorProvider();
        _deferredRoutineActorProvider = deferredRoutineActorProvider();
        _flexConnectInvocationActor = flexConnectInvocationActorProvider();
    }

    //todo: optimize this method to reduce the number of calls to the redis but also contain up to date data
    private async Task<WssdResourceInteractionConfig> GetWssdConfig(Guid wssdResourceId)
    {
        var cacheKey = wssdResourceId.ToString();
    
        if (_configCache.TryGetValue(cacheKey, out var config))
            return config;
        
        var wssdCodeInteractionData = await _redisCacheManager.GetValueAsync(0, cacheKey);
        if (!string.IsNullOrEmpty(wssdCodeInteractionData))
        {
            var result = JsonConvert.DeserializeObject<WssdResourceInteractionConfig>(wssdCodeInteractionData);
            _configCache.TryAdd(cacheKey, result);
            return result;
        }
    
        return null;
    }
    
    // Reuse serializer settings across methods
    private static readonly JsonSerializerSettings _jsonSettings = new JsonSerializerSettings
    {
        ReferenceLoopHandling = ReferenceLoopHandling.Ignore
    };

    // Use faster JSON serialization
    private T DeserializeJson<T>(string json) =>  JsonConvert.DeserializeObject<T>(json, _jsonSettings);
    
    private async Task CleanupSession(string sessionId, Guid wssdResourceId)
    {
        var keys = new[]
        {
            GetMenuCacheKey(sessionId, wssdResourceId),
            GetInputCacheKey(sessionId, wssdResourceId),
            GetNavigationStateKey(sessionId, wssdResourceId)
        };

        // Implement batch delete in Redis manager
        await _redisCacheManager.RemoveValuesAsync(0, keys);
    }
    
    private readonly ConcurrentDictionary<string, AstNode> _expressionCache =  new ConcurrentDictionary<string, AstNode>();
    
    // Declare at class level
    private static readonly Regex PlaceholderPattern =  new Regex(@"\[(.*?)\]", RegexOptions.Compiled);
    
   public async Task<object> HandleInteraction(NaloUssdSessionRequest ussdSessionRequest)
    {
        ussdSessionRequest.SESSIONID = $"{ussdSessionRequest.MSISDN}_{ussdSessionRequest.NETWORK}";
    
        Logger.Info($"ussd_request_from_nalo => {JsonConvert.SerializeObject(ussdSessionRequest)}");
    
        bool isInitiatingPhase = ussdSessionRequest.USERDATA.StartsWith("*");
        bool isError = WssdEngineConsts.NaloUssdErrorCodes.Contains(ussdSessionRequest.USERDATA);
    
        if (isError)
        {
            Logger.Info($"nalo_ussd_error_response triggered");
    
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = ussdSessionRequest.MSISDN,
                USERDATA = string.Empty,
                MSG = "Invalid input !\nSession terminated.",
                MSGTYPE = true,
            };
        }
    
        // Get service code - either from the request or from cache
        string serviceCode;
        if (isInitiatingPhase)
        {
            serviceCode = ussdSessionRequest.USERDATA.Replace("*", string.Empty).Replace("#", string.Empty);
            // Save the service code to the session
            await _redisCacheManager.SetValueAsync(0, ussdSessionRequest.SESSIONID, serviceCode);
        }
        else
        {
            // Get service code from cache for continuation phase
            serviceCode = await _redisCacheManager.GetValueAsync(0, ussdSessionRequest.SESSIONID);
        }
    
        // Reset session expiration time on each input from the user
        if (!string.IsNullOrEmpty(serviceCode))
        {
            await _redisCacheManager.SetExpireTimeAsync(0, ussdSessionRequest.SESSIONID, TimeSpan.FromMinutes(5));
        }
    
        // Fetch the WSSD resource information based on service code
        var wssdResourceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(a => a.UssdCode.Replace("*", string.Empty).Replace("#", string.Empty) == serviceCode && a.IsUssdActive);
    
        if (wssdResourceInfo == null)
        {
            Logger.Info($"WSSD/USSD Service has not been setup or cannot be found !");
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = ussdSessionRequest.MSISDN,
                USERDATA = string.Empty,
                MSG = "WSSD/USSD Service has not been setup or cannot be found",
                MSGTYPE = true,
            };
        }
    
        // Now we have wssdResourceId to use in our cache key
        Guid wssdResourceId = wssdResourceInfo.Id;
    
        if (isInitiatingPhase)
        {
            // Create or update the data dictionary entry for phone number(MSISDN)
            var dataKey = $"input:{ussdSessionRequest.SESSIONID}{wssdResourceId}:data";
            
            
            var initialDataDictionary = new List<DataDictionary>
            {
                new() { Key = "MSISDN", Value = ussdSessionRequest.MSISDN, DataType = "text", Source = "ussd" },
                new() { Key = "NETWORK", Value = ussdSessionRequest.NETWORK, DataType = "text", Source = "ussd" },
            };
    
            await _redisCacheManager.SetValueAsync(0, dataKey, JsonConvert.SerializeObject(initialDataDictionary));
            // Set expiration time for the data dictionary
            await _redisCacheManager.SetExpireTimeAsync(0, dataKey, TimeSpan.FromHours(1));
    
            Logger.Info($"saved_msisdn_to_data_dictionary, @nalo_ussd_interaction_manager => {ussdSessionRequest.MSISDN}");
        }
    
        var wssdCodeInteractionData = await _redisCacheManager.GetValueAsync(0, wssdResourceId.ToString());
    
        if (string.IsNullOrEmpty(wssdCodeInteractionData))
        {
            Logger.Info($"WSSD/USSD Service has not been setup or cannot be found !");
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = ussdSessionRequest.MSISDN,
                USERDATA = string.Empty,
                MSG = "WSSD/USSD Service has not been setup or cannot be found",
                MSGTYPE = true,
            };
        }
    
        var wssdCodeInteractionConfig = JsonConvert.DeserializeObject<WssdResourceInteractionConfig>(wssdCodeInteractionData);
    
        if (isInitiatingPhase)
        {
            Logger.Info($"nalo_ussd_about_to_start_initiation");
            //produce to an actor to  debit ussd session balance here.
            //use an actor to debit session
            //_sessionBalanceActorProvider.Tell(new DebitSessionBalanceDto { ServiceId = wssdResourceId, ServiceType = "ussd"});
            return await HandleInitiation(ussdSessionRequest, wssdResourceInfo, wssdCodeInteractionConfig);
        }

        Logger.Info($"nalo_ussd_start_response_flow");
        return await HandleResponse(ussdSessionRequest, wssdResourceInfo, wssdCodeInteractionConfig);
    }
   
    private async Task<object> HandleInitiation(NaloUssdSessionRequest request, WssdResource wssdResourceInfo, WssdResourceInteractionConfig config)
    {
        Logger.Info($"ussd_initiation started => {request.USERDATA}, sessionId: {request.SESSIONID}, wssdResourceId: {wssdResourceInfo.Id}");

        // get first menu interaction for the ussd code ...
        var orderedMenu = config.MenuList.OrderBy(a => a.Order).ToList();
        
        var primaryMenu = orderedMenu.FirstOrDefault();

        if (primaryMenu == null)
        {
            Logger.Info($"WSSD/USSD Service has not been setup or cannot be found !");
            
            return CreateErrorResponse(request,"WSSD/USSD Service has not been setup or cannot be found");
        }

        var primaryMenuOptions = config.MenuOptionList.Where(x => x.MenuId == primaryMenu.Id && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)).ToList();
        
        var firstOption = primaryMenuOptions.FirstOrDefault();

        if (firstOption == null)
        {
            Logger.Info($"WSSD/USSD Service has not been setup or cannot be found !");
            return CreateErrorResponse(request, "WSSD/USSD Service has not been setup or cannot be found");
        }
        
        Logger.Info($"firstOption nalo_ussd_initiating is invocation => {JsonConvert.SerializeObject(firstOption)}");

        if (firstOption.ActionType == "invocation")
        {
            // Handle invocation action type
            Logger.Info($"invocation triggered in ussd_initiation => {JsonConvert.SerializeObject(firstOption)}");
            
            await UpdateNavigationState(request.SESSIONID,wssdResourceInfo.Id, new NavigationState
            {
                CurrentMenuId = primaryMenu.Id,
                SelectedOptionId = firstOption.Id,
                CurrentActionType = firstOption.ActionType,
                PreviousOptionId = Guid.Empty, 
                IsAwaitingInput = false,
                IsNextSelectionFromExternalDataSource = false, 
                CurrentMenuTitle = primaryMenu.Title, 
                SelectedOptionDisplayText = firstOption.DisplayText
            });
            
            // trigger invocation action handler ....
            
            var dto = new WssdInteractionHandlerDto
            {
                SessionId = request.SESSIONID,
                WssdCodeId = wssdResourceInfo.Id,
                CurrentMenuId = firstOption.MenuId,
                SelectedOptionId = firstOption.Id,
                PreviousOptionId = Guid.Empty,
                ActionType = firstOption.ActionType,
                UserInput = request.USERDATA,
                MobileNo = request.MSISDN,
                Operator = request.NETWORK,
                DataDictionary = new DataDictionary
                {
                    Key =  string.Empty,
                    Value =  string.Empty
                }
            };
            
            return await ActionSwitchHandler(dto, config, request);
        }
        
        if (firstOption.ActionType == "input")
        {
            
            // Save the initial navigation state to cache
            await UpdateNavigationState(request.SESSIONID,wssdResourceInfo.Id, new NavigationState
            {
                CurrentMenuId = primaryMenu.Id,
                SelectedOptionId = firstOption.Id,
                CurrentActionType = firstOption.ActionType,
                PreviousOptionId = Guid.Empty
            });
            
        }

        if (firstOption.ActionType == "display")
        {
            _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
            {
                SessionId = request.SESSIONID,
                WssdResourceId = wssdResourceInfo.Id.ToString(),
                WssdResourceShortName = wssdResourceInfo.WssdShortName,
                ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
                ActionType = "Initialize",
                IsSuccessful = true,
                StartTime = DateTime.UtcNow,
                MenuId = primaryMenu.Id,
                MenuTitle = primaryMenu.Title,
                SelectedOptionId = Guid.Empty,
                SelectedOptionTitle = string.Empty,
                TenantId = wssdResourceInfo.TenantId,
                TenantName = wssdResourceInfo.TenantName,
                EndTime = DateTime.UtcNow,
                Operator = request.NETWORK,
                InteractionSource = WssdEngineConsts.UssdInteractionSource,
                UserInput = $"{wssdResourceInfo.WssdShortName}",
                SystemResponse = $"{firstOption.DisplayOutput}"
            });

            
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = request.MSISDN,
                USERDATA = request.USERDATA,
                MSG = $"{firstOption.DisplayOutput}",
                MSGTYPE = true,
            };
            
        }

        if (firstOption.ActionType == "input")
        {
            _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
            {
                SessionId = request.SESSIONID,
                WssdResourceId = wssdResourceInfo.Id.ToString(),
                WssdResourceShortName = wssdResourceInfo.WssdShortName,
                ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
                ActionType = "Initialize",
                IsSuccessful = true,
                StartTime = DateTime.UtcNow,
                MenuId = primaryMenu.Id,
                MenuTitle = primaryMenu.Title,
                SelectedOptionId = Guid.Empty,
                SelectedOptionTitle = string.Empty,
                TenantId = wssdResourceInfo.TenantId,
                TenantName = wssdResourceInfo.TenantName,
                EndTime = DateTime.UtcNow,
                Operator = request.NETWORK,
                InteractionSource = WssdEngineConsts.UssdInteractionSource,
                UserInput = $"{wssdResourceInfo.WssdShortName}",
                SystemResponse = $"{primaryMenu.Title}\nMessage: {firstOption.DisplayText}\nLabel: {firstOption.InputFieldName}\n Default Value: {firstOption.InputDefaultValue}"
            });
            
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = request.MSISDN,
                USERDATA = request.USERDATA,
                MSG = $"{firstOption.DisplayOutput}",
                MSGTYPE = true,
            };
            
        }
        
        // Save the initial navigation state to cache
        await UpdateNavigationState(request.SESSIONID,wssdResourceInfo.Id, new NavigationState
        {
            CurrentMenuId = primaryMenu.Id,
            SelectedOptionId = Guid.Empty,
            CurrentActionType = firstOption.ActionType,
            PreviousOptionId = Guid.Empty,
            IsAwaitingInput = false,
            IsNextSelectionFromExternalDataSource = false,
        });

        var numberedDisplayText = string.Join("\n", primaryMenuOptions.Select((option, index) => $"{index + 1}. {option.DisplayText}"));

        _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
        {
            SessionId = request.SESSIONID,
            WssdResourceId = wssdResourceInfo.Id.ToString(),
            WssdResourceShortName = wssdResourceInfo.WssdShortName,
            ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
            ActionType = "Initialize",
            IsSuccessful = true,
            StartTime = DateTime.UtcNow,
            MenuId = primaryMenu.Id,
            MenuTitle = primaryMenu.Title,
            SelectedOptionId = Guid.Empty,
            SelectedOptionTitle = string.Empty,
            TenantId = wssdResourceInfo.TenantId,
            TenantName = wssdResourceInfo.TenantName,
            EndTime = DateTime.UtcNow,
            InteractionSource = WssdEngineConsts.UssdInteractionSource,
            Operator = request.NETWORK,
            UserInput = $"{wssdResourceInfo.WssdShortName}",
            MobileNo = request.MSISDN,
            SystemResponse = $"{primaryMenu.Title}\n{numberedDisplayText}"
        });

        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = $"{primaryMenu.Title}\n{numberedDisplayText}",
            MSGTYPE = true,
        };
        
    }
     
    private async Task<object> HandleResponse(NaloUssdSessionRequest request,  WssdResource wssdResourceInfo, WssdResourceInteractionConfig config)
    {
        //if the request.USERDATA is empty, return
        if (string.IsNullOrEmpty(request.USERDATA))
        {
            return CreateErrorResponse(request, "Invalid input !\nSession terminated.");
        }
        
        // Handle back navigation
        if (request.USERDATA == "0" || request.USERDATA == "00")
        {
            return await HandleBackNavigation(request, wssdResourceInfo, config);
        }
        
        // Handle pagination navigation
        if (request.USERDATA == "98") // Next page
        {
            return await HandlePaginationNavigation(request, wssdResourceInfo, config, true);
        }

        if (request.USERDATA == "99") // Previous page
        {
            return await HandlePaginationNavigation(request, wssdResourceInfo, config, false);
        }

        var navigationState = await GetNavigationState(request.SESSIONID, wssdResourceInfo.Id);
    
        if (navigationState == null)
        {
            // No existing navigation state - handle as new session initiation
            Logger.Info("No existing navigation state - handle as new session initiation");
            return await HandleInitiation(request, wssdResourceInfo, config);
        }

        Logger.Info($"navigationState before ProcessCurrentAction {request.SESSIONID}, navigationState: {JsonConvert.SerializeObject(navigationState)}");
        
        //Handle the current action based on navigation state ...
        var currentMenu = config.MenuList.FirstOrDefault(x => x.IsActive() && x.Id == navigationState.CurrentMenuId);

        if (currentMenu == null)
        {
            return CreateErrorResponse(request, "Current menu not found.");
        }
        
        if (navigationState.IsAwaitingInput)
        {
            //handle user input...
            Logger.Info($"flow awaiting input sessionId: {request.SESSIONID} => ");

            var awaitingInputOption =
                config.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.MenuId == navigationState.CurrentMenuId && x.Id == navigationState.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

            if (awaitingInputOption == null)
            {
                return CreateErrorResponse(request, "Invalid option !");
            }
            
            var nextMenuOption =
                config.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.Id == awaitingInputOption.NextMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

            if (nextMenuOption == null)
            {
                return CreateErrorResponse(request, "Next menu option not found.");
            }

            var dto = new WssdInteractionHandlerDto
            {
                SessionId = request.SESSIONID,
                WssdCodeId = wssdResourceInfo.Id,
                CurrentMenuId = navigationState.CurrentMenuId,
                SelectedOptionId = awaitingInputOption.NextMenuId,
                PreviousOptionId = navigationState.SelectedOptionId,
                ActionType = nextMenuOption.ActionType,
                UserInput = request.USERDATA,
                MobileNo = request.MSISDN,
                Operator = request.NETWORK,
                DataDictionary = new DataDictionary
                {
                    Key = awaitingInputOption.InputFieldName,
                    Value = request.USERDATA, 
                    DataType = awaitingInputOption.InputType, 
                    Source = "ussd"
                    
                }
            };
            
            //update the navigation state ...
            navigationState.IsAwaitingInput = false;
            await UpdateNavigationState(request.SESSIONID, wssdResourceInfo.Id, navigationState);
            
            return await ActionSwitchHandler(dto, config, request);
        }
        else
        {
            // if the parent menu of the currentMenuOptions has its DataSource set to api-response
            if (navigationState.IsNextSelectionFromExternalDataSource)
            {
                Logger.Info($"flow awaiting external data source sessionId: {request.SESSIONID} => {request.USERDATA}");
                
                //check the menu-meta cache
                string menuMetaDataCacheKey = $"menuMetaData:{request.SESSIONID}{wssdResourceInfo.Id}";
                var menuMetaDataCache = await _redisCacheManager.GetValueAsync(0, menuMetaDataCacheKey);
            
                List<ExternalMenuMetaData> externalMenuList = string.IsNullOrEmpty(menuMetaDataCache) ? new List<ExternalMenuMetaData>() : JsonConvert.DeserializeObject<List<ExternalMenuMetaData>>(menuMetaDataCache);
            
                var selectedMenuOption = externalMenuList.FirstOrDefault(x => x.Id == request.USERDATA.Trim());

                Logger.Info($"selectedMenuOption from external_data_source sessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(selectedMenuOption)}");
                
                if (selectedMenuOption == null)
                {
                    
                    return new NaloUssdSessionResponse
                    {
                        USERID = "Rhyolite",
                        MSISDN = request.MSISDN,
                        USERDATA = request.USERDATA,
                        MSG = "Invalid option.",
                        MSGTYPE = false,
                    };
                    
                }
                
                //retrieve the original menu template
                // navigate with its next menu prop
                // set the Id to data dictionary with the key as the input field name
                
                //log the navigation state before processing the current action
                    Logger.Info($"navigationState at_resolving_ext_menu {request.SESSIONID}, =>: {JsonConvert.SerializeObject(navigationState)}");
                 
                var previousOptionInExtMenuContext =
                    config.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.Id == navigationState.PreviousOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

                Logger.Info($"previousOptionInExtMenuContext sessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(previousOptionInExtMenuContext)}");
                
                if (previousOptionInExtMenuContext == null)
                {
                   return CreateErrorResponse(request, "An unexpected error occurred while processing your request.\nPlease try again later or contact support.");
                }
                
                
                var nextMenu = config.MenuList.FirstOrDefault(x => x.Id == previousOptionInExtMenuContext.NextMenuId);
                
                if (nextMenu == null)
                {
                    return CreateErrorResponse(request, "An unexpected error occurred while processing your request.\nPlease try again later or contact support.");
                }

                var menuOptionTemplate = config.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.MenuId == nextMenu.Id && !x.IsVisibleOnDemand && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

                Logger.Info($"menuOptionTemplate sessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(menuOptionTemplate)}");
                
                if (menuOptionTemplate == null)
                {
                    return CreateErrorResponse(request, "An unexpected error occurred while processing your request.\nPlease try again later or contact support.");
                }
                
                var transformedSelectedMenuOption = new WssdMenuOption
                {
                    Id = menuOptionTemplate.Id,
                    MenuId = navigationState.CurrentMenuId,
                    DisplayText = selectedMenuOption.DisplayText,
                    ActionType = "menu-input",
                    DisplayType = "text",
                    InputType = "short-text",
                    InputFieldName = menuOptionTemplate.InputFieldName,
                    InputDefaultValue = selectedMenuOption.Id,
                    NextMenuId = menuOptionTemplate.NextMenuId,
                    IsVisibleOnDemand = false,
                    ExecutionScope = menuOptionTemplate.ExecutionScope,
                };
                
                Logger.Info($"transformedSelectedMenuOption sessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(transformedSelectedMenuOption)}");
                
                // Save the selected option to the data dictionary
                var dataDictionary = await _redisCacheManager.GetValueAsync(0, GetDataDictionaryKey(request.SESSIONID, wssdResourceInfo.Id));
                var dataDictionaryList = string.IsNullOrEmpty(dataDictionary) ? new List<DataDictionary>() : JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionary);
                var existingData = dataDictionaryList.FirstOrDefault(x => x.Key == transformedSelectedMenuOption.InputFieldName);
                if (existingData != null)
                {
                    existingData.Value = selectedMenuOption.Id;
                }
                else
                {
                    dataDictionaryList.Add(new DataDictionary
                    {
                        Key = transformedSelectedMenuOption.InputFieldName,
                        Value = selectedMenuOption.Id,
                        DataType = "text",
                        Source = "ussd"
                    });
                }
                
                Logger.Info($"saved_selected_option_to_data_dictionary: key:{transformedSelectedMenuOption.InputFieldName}, value: {selectedMenuOption.Id} => {JsonConvert.SerializeObject(dataDictionaryList)}");
                await _redisCacheManager.SetValueAsync(0, GetDataDictionaryKey(request.SESSIONID, wssdResourceInfo.Id), JsonConvert.SerializeObject(dataDictionaryList));
                
                // Update navigation state with the selected option
                navigationState.PreviousOptionId = navigationState.SelectedOptionId;
                navigationState.PreviousMenuId = navigationState.CurrentMenuId;
                navigationState.CurrentMenuId = transformedSelectedMenuOption.NextMenuId;
                navigationState.SelectedOptionId = transformedSelectedMenuOption.Id;
                navigationState.CurrentActionType = transformedSelectedMenuOption.ActionType;
                navigationState.IsAwaitingInput = false; // Reset awaiting input state
                navigationState.IsNextSelectionFromExternalDataSource = false; // Reset external data source state
                navigationState.CurrentMenuTitle = selectedMenuOption.DisplayText;
                navigationState.SelectedOptionDisplayText = selectedMenuOption.DisplayText;
                // Save the updated navigation state
                await UpdateNavigationState(request.SESSIONID, wssdResourceInfo.Id, navigationState);
                
                Logger.Info($"navigationState after ProcessCurrentAction {request.SESSIONID}, navigationState: {JsonConvert.SerializeObject(navigationState)}");
              
                var extDto = new WssdInteractionHandlerDto
                {
                    SessionId = request.SESSIONID,
                    WssdCodeId = wssdResourceInfo.Id,
                    CurrentMenuId = navigationState.CurrentMenuId,
                    SelectedOptionId = transformedSelectedMenuOption.Id,
                    PreviousOptionId = navigationState.PreviousOptionId,
                    ActionType = transformedSelectedMenuOption.ActionType,
                    UserInput = request.USERDATA,
                    MobileNo = request.MSISDN,
                    Operator = request.NETWORK,
                    DataDictionary = new DataDictionary
                    {
                        Key = transformedSelectedMenuOption.InputFieldName,
                        Value = selectedMenuOption.Id, 
                        DataType = "text", 
                        Source = "ussd"
                    }
                };
                
                // Log the interaction
                _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
                {
                    SessionId = request.SESSIONID,
                    WssdResourceId = wssdResourceInfo.Id.ToString(),
                    WssdResourceShortName = wssdResourceInfo.WssdShortName,
                    ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
                    ActionType = "ExternalDataSourceSelection",
                    IsSuccessful = true,
                    StartTime = DateTime.UtcNow,
                    MenuId = navigationState.CurrentMenuId,
                    MenuTitle = navigationState.CurrentMenuTitle,
                    SelectedOptionId = transformedSelectedMenuOption.Id,
                    SelectedOptionTitle = transformedSelectedMenuOption.DisplayText,
                    TenantId = wssdResourceInfo.TenantId,
                    TenantName = wssdResourceInfo.TenantName,
                    EndTime = DateTime.UtcNow,
                    InteractionSource = WssdEngineConsts.UssdInteractionSource,
                    Operator = request.NETWORK,
                    UserInput = $"{wssdResourceInfo.WssdShortName}",
                    MobileNo = request.MSISDN,
                    SystemResponse = $"{navigationState.CurrentMenuTitle}\n{transformedSelectedMenuOption.DisplayText}"
                });
                
                // Handle the action switch for the transformed selected option
                return await ActionSwitchHandler(extDto, config, request);
                
            }
            
            int selectedIndex = int.Parse(request.USERDATA) - 1;

            var currentMenuOptions = config.MenuOptionList.Where(x => x.IsActive() && x.MenuId == navigationState.CurrentMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)).ToList();
     
            Logger.Info($"currentMenuOptions sessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(currentMenuOptions)}");
  
            
            if (selectedIndex < 0 || selectedIndex >= currentMenuOptions.Count)
            {
                Logger.Info($"Invalid selection, index out of range: {selectedIndex} for session: {request.SESSIONID}");
                // Re-initialize the session
                await CleanupSession(request.SESSIONID, wssdResourceInfo.Id);
                // Start the flow from the beginning
                return await HandleInitiation(request, wssdResourceInfo, config);
            }

            var selectedOption = currentMenuOptions[selectedIndex];
            
            Logger.Info($"selectedOption SessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(selectedOption)}");
            
            
            //if the selected option is a display action. 
            if (selectedOption is { ActionType: "display" })
            {
                var displayActionInteractionHandlerDto = new WssdInteractionHandlerDto
                {
                    MobileNo = request.MSISDN,
                    UserInput = request.USERDATA,
                    SessionId = request.USERDATA,
                    CurrentMenuId = navigationState.CurrentMenuId,
                    SelectedOptionId = selectedOption.Id,
                    WssdCodeId = wssdResourceInfo.Id, 
                    SelectedOptionIndex = selectedIndex, 
                    ActionType = selectedOption.ActionType,
                    Operator = request.NETWORK,
                    PreviousOptionId = navigationState.SelectedOptionId,
                    IsHandlingFallback = false,
                    DataDictionary =  new DataDictionary
                    {
                        Key =  string.Empty,
                        Value =  string.Empty
                    }

                };

                return await ActionSwitchHandler(displayActionInteractionHandlerDto, config, request);

            }
            
            var nextUssdMenu = config.MenuList.FirstOrDefault(x => x.IsActive() && x.Id == selectedOption?.NextMenuId);

            Logger.Info($"nextUssdMenu SessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(nextUssdMenu)}");
            
            if (nextUssdMenu == null)
            {
                throw new UserFriendlyException(400, "Next menu not found.");
            }
            var nextUssdMenuOptions = config.MenuOptionList.Where(x => x.IsActive() && x.MenuId == nextUssdMenu.Id && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)).ToList();

            Logger.Info($"nextUssdMenuOptions, SessionId: {request.SESSIONID} => {JsonConvert.SerializeObject(nextUssdMenuOptions)}");
            
            //handle cases for invocation,
            
            // check if the selected option has a default value behind the scenes
            // if the selected option has a default value, save it to the data dictionary with the key as the input field name
            
            if (!string.IsNullOrEmpty(selectedOption.InputDefaultValue))
            {
                var dataDictionary = await _redisCacheManager.GetValueAsync(0, GetDataDictionaryKey(request.SESSIONID, wssdResourceInfo.Id));
                var dataDictionaryList = string.IsNullOrEmpty(dataDictionary) ? new List<DataDictionary>() : JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionary);
                
                var existingData = dataDictionaryList.FirstOrDefault(x => x.Key == selectedOption.InputFieldName);
                if (existingData != null)
                {
                    existingData.Value = selectedOption.InputDefaultValue;
                }
                else
                {
                    dataDictionaryList.Add(new DataDictionary
                    {
                        Key = selectedOption.InputFieldName,
                        Value = selectedOption.InputDefaultValue,
                        DataType = "text",
                        Source = "ussd"
                    });
                }
                
                Logger.Info($"saved_default_value_to_data_dictionary: key:{selectedOption.InputFieldName}, value: {selectedOption.InputDefaultValue} => {JsonConvert.SerializeObject(dataDictionaryList)}");
                 
                await _redisCacheManager.SetValueAsync(0, GetDataDictionaryKey(request.SESSIONID, wssdResourceInfo.Id), JsonConvert.SerializeObject(dataDictionaryList));
            }
            
            // Update navigation state with previous option and menu ...
            navigationState.PreviousOptionId = navigationState.SelectedOptionId;
            navigationState.PreviousMenuId = navigationState.CurrentMenuId;
            navigationState.CurrentMenuTitle = currentMenu.Title;

            navigationState.CurrentMenuId = selectedOption.NextMenuId;
            navigationState.SelectedOptionId = selectedOption.Id;
            navigationState.CurrentActionType = selectedOption.ActionType;

            //Save the updated navigation state...
            await UpdateNavigationState(request.SESSIONID, wssdResourceInfo.Id, navigationState);

            Logger.Info($"navigationState after ProcessCurrentAction {request.SESSIONID}, navigationState: {JsonConvert.SerializeObject(navigationState)}");
        
            var dto = new WssdInteractionHandlerDto
            {
                SessionId = request.SESSIONID,
                WssdCodeId = wssdResourceInfo.Id,
                CurrentMenuId = navigationState.CurrentMenuId,
                SelectedOptionId = navigationState.SelectedOptionId,
                PreviousOptionId = navigationState.PreviousOptionId,
                ActionType = navigationState.CurrentActionType,
                UserInput = request.USERDATA,
                MobileNo = request.MSISDN,
                Operator = request.NETWORK,
                DataDictionary = new DataDictionary
                {
                    Key =  !string.IsNullOrEmpty(selectedOption.InputFieldName) && navigationState.IsAwaitingInput ? selectedOption.InputFieldName : string.Empty,
                    Value = !string.IsNullOrEmpty(selectedOption.InputFieldName) && navigationState.IsAwaitingInput ? request.USERDATA : string.Empty
                }
            };

            return await ActionSwitchHandler(dto, config, request);
        }
        

    }
     
    private async Task<object> HandleBackNavigation(NaloUssdSessionRequest request, WssdResource wssdResourceInfo, WssdResourceInteractionConfig config)
    {
        var navigationState = await GetNavigationState(request.SESSIONID, wssdResourceInfo.Id);
        if (navigationState == null || request.USERDATA == "00")
        {
            // Return to main menu ...
            return await HandleInitiation(request, wssdResourceInfo, config);
        }

        //Find the previous menu of the current menu/option and navigate to it ...
        
        var previousMenu = config.MenuList.FirstOrDefault(m =>  m.Id == navigationState.PreviousMenuId & m.IsActive());
        
        if (previousMenu == null)
        {
            Logger.Info($"hubtel_ussd_Previous menu not found. @HandleInitiation has been called");
            
            return await HandleInitiation(request, wssdResourceInfo, config);
        }

        var previousMenuOptions = config.MenuOptionList
            .Where(x => x.IsActive() && x.MenuId == previousMenu.Id && !x.IsVisibleOnDemand)
            .ToList();
        
        // Determine if this is the first menu in the flow
        var orderedMenus = config.MenuList.OrderBy(a => a.Order).ToList();
        bool isFirstMenu = orderedMenus.FirstOrDefault()?.Id == previousMenu.Id;

        // Update navigation state...
        await UpdateNavigationState(request.SESSIONID, wssdResourceInfo.Id, new NavigationState
        {
            CurrentMenuId = previousMenu.Id,
            SelectedOptionId = Guid.Empty,
            PreviousOptionId = navigationState.SelectedOptionId,
            CurrentActionType = "menu"
        });
        
        Logger.Info($"hubtel_ussd_@CreateMenuResponse has been called");

        return CreateMenuResponse(request, previousMenu, previousMenuOptions, !isFirstMenu);
    }
    
    private async Task<object> HandlePaginationNavigation(NaloUssdSessionRequest request, WssdResource wssdResourceInfo, WssdResourceInteractionConfig config, bool isNextPage)
    {
        var navigationState = await GetNavigationState(request.SESSIONID, wssdResourceInfo.Id);
        if (navigationState == null)
        {
            return await HandleInitiation(request, wssdResourceInfo, config);
        }
        
        // If CurrentPage property doesn't exist in your NavigationState, you can store it separately
        int currentPage = 1;
        var pageCache = await _redisCacheManager.GetValueAsync(0, $"ussd-page:{request.SESSIONID}{wssdResourceInfo.Id}");
        
        if (!string.IsNullOrEmpty(pageCache) && int.TryParse(pageCache, out int cachedPage))
        {
            currentPage = cachedPage;
        }
        
        // Update page based on navigation
        if (isNextPage)
        {
            currentPage++;
        }
        else
        {
            currentPage = Math.Max(1, currentPage - 1);
        }
        
        // Save updated page to cache.
        await _redisCacheManager.SetValueAsync(0, $"ussd-page:{request.SESSIONID}{wssdResourceInfo.Id}", currentPage.ToString());
        
        // Get current menu
        var currentMenu = config.MenuList.FirstOrDefault(m => m.Id == navigationState.CurrentMenuId && m.IsActive());
        if (currentMenu == null)
        {
            return await HandleInitiation(request, wssdResourceInfo, config);
        }
        
        // Get menu options
        var menuOptions = config.MenuOptionList
            .Where(x => x.IsActive() && x.MenuId == currentMenu.Id && !x.IsVisibleOnDemand)
            .ToList();
        
        // Determine if this is the first menu in the flow (for back navigation option)
        var orderedMenus = config.MenuList.OrderBy(a => a.Order).ToList();
        bool isFirstMenu = orderedMenus.FirstOrDefault()?.Id == currentMenu.Id;
        
        // Create paginated response
        return CreatePaginatedMenuResponse(request, currentMenu, menuOptions, !isFirstMenu, DefaultPageSize, currentPage);
    }
    
    private async Task<bool> HandleInputConfirmation(NaloUssdSessionRequest request,
        NavigationState navigationState,
        WssdResource wssdResourceInfo,
        WssdResourceInteractionConfig config)
    {
        var currentOption = config.MenuOptionList.FirstOrDefault(x => x.Id == navigationState.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
        if (currentOption == null || string.IsNullOrEmpty(currentOption.InputDefaultValue))
        {
            return false;
        }

        if (request.USERDATA == "1") // Confirm
        {
            // Save the default value
            await SaveInputValue(request.SESSIONID, wssdResourceInfo.Id, currentOption.InputFieldName, 
                currentOption.InputDefaultValue);
            return true;
        }

        if (request.USERDATA == "2") // Abort
        {
            return await HandleTerminateAction(new WssdInteractionHandlerDto
            {
                SessionId = request.SESSIONID,
                WssdCodeId = wssdResourceInfo.Id
            }) != null;
        }

        return false;
    }
    
     
    private async Task<object> ProcessNextAction(NaloUssdSessionRequest request, NavigationState navigationState, WssdResource wssdResourceInfo, WssdResourceInteractionConfig config)
    {
        var currentOption = config.MenuOptionList.FirstOrDefault(x => x.Id == navigationState.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
        if (currentOption == null)
        {
            return CreateErrorResponse(request, "Current option not found.");
        }

        var nextOption = config.MenuOptionList.FirstOrDefault(x => x.Id == currentOption.NextMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
        if (nextOption == null)
        {
            return CreateErrorResponse(request, "Next option not found.");
        }

        // Update navigation state
        navigationState.CurrentMenuId = nextOption.MenuId;
        navigationState.SelectedOptionId = nextOption.Id;
        navigationState.CurrentActionType = nextOption.ActionType;
        await UpdateNavigationState(request.SESSIONID, wssdResourceInfo.Id, navigationState);

        // Handle the action type of the next option
        var dto = new WssdInteractionHandlerDto
        {
            SessionId = request.SESSIONID,
            WssdCodeId = wssdResourceInfo.Id,
            CurrentMenuId = nextOption.MenuId,
            SelectedOptionId = nextOption.Id,
            PreviousOptionId = currentOption.Id,
            ActionType = nextOption.ActionType,
            UserInput = request.USERDATA,
            MobileNo = request.MSISDN,
            Operator = request.NETWORK,
            DataDictionary = new DataDictionary
            {
                Key = nextOption.InputFieldName,
                Value = request.USERDATA
            }
        };

        return await ActionSwitchHandler(dto, config, request);
    }
    
    private async Task<NavigationState> GetNavigationState(string sessionId, Guid wssdResourceId)
    {
        var stateCache = await _redisCacheManager.GetValueAsync(0, 
            $"ussd-navigation-state:{sessionId}{wssdResourceId}");
        return string.IsNullOrEmpty(stateCache) 
            ? null 
            : JsonConvert.DeserializeObject<NavigationState>(stateCache);
    }

    private async Task UpdateNavigationState(string sessionId, Guid wssdResourceId, NavigationState state)
    {
        await _redisCacheManager.SetValueAsync(0, 
            $"ussd-navigation-state:{sessionId}{wssdResourceId}",
            JsonConvert.SerializeObject(state));
        
        await _redisCacheManager.SetExpireTimeAsync(0, $"ussd-navigation-state:{sessionId}{wssdResourceId}", TimeSpan.FromMinutes(30));
    }
    
    private async Task SaveInputValue(string sessionId, Guid wssdResourceId, string key, string value)
    {
        var dataKey = $"input:{sessionId}{wssdResourceId}:data";
        var existingData = await _redisCacheManager.GetValueAsync(0, dataKey);
        var dataDictionary = string.IsNullOrEmpty(existingData)
            ? new List<DataDictionary>()
            : JsonConvert.DeserializeObject<List<DataDictionary>>(existingData);

        var existing = dataDictionary.FirstOrDefault(x => x.Key == key);
        if (existing != null)
        {
            existing.Value = value;
        }
        else
        {
            dataDictionary.Add(new DataDictionary { Key = key, Value = value });
        }

        await _redisCacheManager.SetValueAsync(0, dataKey, JsonConvert.SerializeObject(dataDictionary));
    }

    private async Task SaveDataDictionaryValues(string sessionId, Guid wssdResourceId, Dictionary<string, object> values)
    {
        if (values == null || !values.Any())
        {
            return;
        }

        var dataKey = $"input:{sessionId}{wssdResourceId}:data";
        var existingData = await _redisCacheManager.GetValueAsync(0, dataKey);
        var dataDictionary = string.IsNullOrEmpty(existingData)
            ? new List<DataDictionary>()
            : JsonConvert.DeserializeObject<List<DataDictionary>>(existingData);

        foreach (var item in values)
        {
            var existing = dataDictionary.FirstOrDefault(x => x.Key == item.Key);
            if (existing != null)
            {
                existing.Value = item.Value?.ToString();
            }
            else
            {
                dataDictionary.Add(new DataDictionary { Key = item.Key, Value = item.Value?.ToString() });
            }
        }

        await _redisCacheManager.SetValueAsync(0, dataKey, JsonConvert.SerializeObject(dataDictionary));
    }
    
    private NaloUssdSessionResponse CreateMenuResponse(NaloUssdSessionRequest request, ProposedMenu menu, List<WssdMenuOption> options, bool includeBackOptions = true)
    {
        Logger.Info($"CreateMenuResponse called for sessionId: {request.SESSIONID}");
        
        // Get current page (default to 1)
        int currentPage = 1;
            
        // Check if we should use pagination (more than DefaultPageSize items)
        if (options.Count > DefaultPageSize)
        {
            return CreatePaginatedMenuResponse(request, menu, options, includeBackOptions, DefaultPageSize, currentPage);
        }
        
        // Implementation for small menus
        var displayOptions = options.Select((option, index) => $"{index + 1}. {option.DisplayText}").ToList();
    
        if (includeBackOptions)
        {
            displayOptions.Add("0. Back");
            displayOptions.Add("00. Main Menu");
        }
        
        var numberedDisplayText = string.Join("\n", displayOptions);

        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG =  $"{menu.Title}\n{numberedDisplayText}",
            MSGTYPE = true,
        };
        
    }
    
    private NaloUssdSessionResponse CreateErrorResponse(NaloUssdSessionRequest request, string message)
    {
        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = message,
            MSGTYPE = true,
        };
        
    }
    
    
    private NaloUssdSessionResponse CreateDefaultResponse(NaloUssdSessionRequest request, WssdResource wssdResourceInfo)
    {
        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = "Welcome to the Hubtel USSD Interaction Manager.\nInteraction contract malformed",
            MSGTYPE = false,
        };
        
         
    }

    private NaloUssdSessionResponse CreatePaginatedMenuResponse(NaloUssdSessionRequest request, 
            ProposedMenu menu, List<WssdMenuOption> options, 
            bool includeBackOptions = true, int pageSize = DefaultPageSize, int currentPage = 1)
        {
            // Calculate pagination details
            int totalOptions = options.Count;
            int totalPages = (int)Math.Ceiling(totalOptions / (double)pageSize);
            
            // Ensure current page is valid
            currentPage = Math.Max(1, Math.Min(currentPage, totalPages));
            
            Logger.Info($"currentPage: {currentPage}, totalPages: {totalPages}, pageSize: {pageSize}");
            
            // Get options for current page
            var pagedOptions = options
                .Skip((currentPage - 1) * pageSize)
                .Take(pageSize)
                .ToList();
            
            Logger.Info($"pagedOptions: {JsonConvert.SerializeObject(pagedOptions)}");
            
            // Create display text for current page options
            var displayOptions = pagedOptions.Select((option, index) => 
                $"{(currentPage - 1) * pageSize + index + 1}. {option.DisplayText}").ToList();
            
            // Add pagination controls if needed
            if (totalPages > 1)
            {
                // Add "Previous Page" option if not on first page
                if (currentPage > 1)
                {
                    displayOptions.Add(PrevPageOption);
                }
                
                // Add "Next Page" option if not on last page
                if (currentPage < totalPages)
                {
                    displayOptions.Add(NextPageOption);
                }
            }
            
            Logger.Info($"displayOptions: {JsonConvert.SerializeObject(displayOptions)}");
            
            // Add back navigation options
            if (includeBackOptions)
            {
                displayOptions.Add("0. Back");
                displayOptions.Add("00. Main Menu");
            }
            
            // Join all options into single text with page indicator
            var numberedDisplayText = string.Join("\n", displayOptions);
            string pageIndicator = totalPages > 1 ? $"Page {currentPage}/{totalPages}\n" : "";
            
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = request.MSISDN,
                USERDATA = request.USERDATA,
                MSG = $"{menu.Title}\n{pageIndicator}{numberedDisplayText}",
                MSGTYPE = true,
            };
        }
    
    private async Task<object> ActionSwitchHandler(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {
        // Handle data dictionary updates
        await UpdateDataDictionary(dto, wssdCodeInteractionConfig);
        
        // Get the current menu option
        var currentMenuOption = wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => 
            x.IsActive() && x.Id == dto.SelectedOptionId && 
            (string.IsNullOrEmpty(x.ExecutionScope) || 
             x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || 
             x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)); 
        
        
        if (currentMenuOption == null)
        {
            //check the menu-meta cache
            string menuMetaDataCacheKey = $"menuMetaData:{dto.SessionId}{dto.WssdCodeId}";
            var menuMetaDataCache = await _redisCacheManager.GetValueAsync(0, menuMetaDataCacheKey);
            
            List<ExternalMenuMetaData> externalMenuList = string.IsNullOrEmpty(menuMetaDataCache)
                ? new List<ExternalMenuMetaData>()
                : JsonConvert.DeserializeObject<List<ExternalMenuMetaData>>(menuMetaDataCache);
            
            var selectedMenuOption = externalMenuList.FirstOrDefault(x => x.UniqueId == dto.SelectedOptionId);

            if (selectedMenuOption == null)
            {
                throw new UserFriendlyException(400, "Invalid option.");
            }
            
            var menuOptionTemplate =
                wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.MenuId == dto.CurrentMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

            if (menuOptionTemplate == null)
            {
                throw new UserFriendlyException(400, "Menu option template not found.");
            }
            
            
            currentMenuOption = new WssdMenuOption
            {
                Id = menuOptionTemplate.Id,
                MenuId = dto.CurrentMenuId,
                DisplayText = selectedMenuOption.DisplayText,
                ActionType = "menu-input",
                DisplayType = "text",
                InputType = "short-text",
                InputFieldName = menuOptionTemplate.InputFieldName,
                InputDefaultValue = selectedMenuOption.Id,
                NextMenuId = menuOptionTemplate.NextMenuId,
                IsVisibleOnDemand = false,
                ExecutionScope = menuOptionTemplate.ExecutionScope,
            };
            
        }
        
        
        await CacheContextVariablesIfPresent(dto, currentMenuOption);
        
        // Handle flow control for previous action
        if (dto.PreviousOptionId != Guid.Empty)
        {
            var previousAction = wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == dto.PreviousOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
            if (previousAction == null)
            {
                throw new UserFriendlyException(400, "Navigation failed.");
            }
    
            // Set retry count if not already set
            await SetRetryCountToCache(dto.SessionId, dto.PreviousOptionId, previousAction.Retries);
            
            // Check flow control condition if expression exists
            if (!string.IsNullOrEmpty(previousAction.FlowControlExpression))
            {
                bool canFlowProceed = await EvaluateFlowControl(dto, previousAction);
                
                if (!canFlowProceed)
                {
                    Logger.Info($"flow_control_cannot_proceed @PreviousOption => {previousAction.FlowControlExpression}");
                   
                    Logger.Info($"initiating retry... ");
                    // Handle retry logic
                    var retryCount = await GetRetryCount(dto.SessionId, dto.PreviousOptionId);
                    Logger.Info($"retryCount => {retryCount}");
                    
                    if (retryCount > 0)
                    {
                        await DecrementRetryCount(dto.SessionId, dto.PreviousOptionId);
                        return await FlowControlRetryHandler(dto, previousAction, wssdCodeInteractionConfig, request);
                    }
                    else
                    {
                        //route to the fallback action
                        var fallbackAction = wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == previousAction.FlowControlFallbackActionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
                        if (fallbackAction == null)
                        {
                            throw new UserFriendlyException(400, "Flow control fallback action not found.");
                        }
                        Logger.Info($"flow_control_fallback_action => {fallbackAction.DisplayText}");
                        // Update navigation state
                        await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
                        {
                            CurrentMenuId = fallbackAction.MenuId,
                            SelectedOptionId = fallbackAction.Id,
                            PreviousOptionId = dto.PreviousOptionId,
                            CurrentActionType = fallbackAction.ActionType,
                            IsHandlingFallback = true
                        });
                        
                        // Create DTO for fallback action
                        
                        var fallbackDto = new WssdInteractionHandlerDto
                        {
                            SessionId = dto.SessionId,
                            WssdCodeId = dto.WssdCodeId,
                            CurrentMenuId = fallbackAction.MenuId,
                            SelectedOptionId = fallbackAction.Id,
                            PreviousOptionId = dto.PreviousOptionId,
                            ActionType = fallbackAction.ActionType,
                            UserInput = dto.UserInput,
                            MobileNo = dto.MobileNo,
                            Operator = dto.Operator,
                            IsHandlingFallback = true
                        };

                        // Process the fallback action directly based on its action type instead of calling ActionSwitchHandler
                        return fallbackAction.ActionType switch
                        {
                            "menu" => await HandleMenuAction(fallbackDto, wssdCodeInteractionConfig, request),
                            "menu-input" => await HandleSelectableInputAction(fallbackDto, wssdCodeInteractionConfig, request),
                            "display" => await HandleDisplayAction(fallbackDto, wssdCodeInteractionConfig, request),
                            "input" => await HandleInputAction(fallbackDto, wssdCodeInteractionConfig, request),
                            "invocation" => await HandleApiCallAction(fallbackDto, wssdCodeInteractionConfig, request),
                            "terminate" => await HandleTerminateAction(fallbackDto),
                            "deferred-routine" => await HandleDeferredRoutineAction(fallbackDto, wssdCodeInteractionConfig, request),
                            _ => throw new UserFriendlyException(400, "Invalid fallback action type")
                        };
                    }

                }
            }
        }
    
        // Route to appropriate action handler based on action type
        return dto.ActionType switch
        {
            "menu" => await HandleMenuAction(dto, wssdCodeInteractionConfig, request),
            "menu-input" => await HandleSelectableInputAction(dto, wssdCodeInteractionConfig, request),
            "display" => await HandleDisplayAction(dto, wssdCodeInteractionConfig, request),
            "input" => await HandleInputAction(dto, wssdCodeInteractionConfig, request),
            "invocation" => await HandleApiCallAction(dto, wssdCodeInteractionConfig, request),
            "terminate" => await HandleTerminateAction(dto),
            "deferred-routine" => await HandleDeferredRoutineAction(dto, wssdCodeInteractionConfig, request),
            _ => throw new UserFriendlyException(400, "Invalid action type")
        };
    }
    private async Task UpdateDataDictionary(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig)
    {
        if (dto.DataDictionary == null || string.IsNullOrEmpty(dto.DataDictionary.Key) || dto.DataDictionary.Value == null)
        {
            Logger.Info($"Data dictionary is empty or invalid for sessionId: {dto.SessionId}");
            return;
        }
        
        dto.DataDictionary.Source = "user-input";
        
        // Retrieve data dictionary from cache
        var dataDictionaryCache = await _redisCacheManager.GetValueAsync(0, GetDataDictionaryKey(dto.SessionId, dto.WssdCodeId));
        
        List<DataDictionary> dataDictionaryCacheList = string.IsNullOrEmpty(dataDictionaryCache)
            ? new List<DataDictionary>()
            : JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionaryCache);
        
        // Update existing entry or add new one
        var existingDataDictionary = dataDictionaryCacheList.FirstOrDefault(x => x.Key == dto.DataDictionary.Key);
        if (existingDataDictionary != null)
        {
            existingDataDictionary.Value = dto.DataDictionary.Value;
        }
        else
        {
            dataDictionaryCacheList.Add(dto.DataDictionary);
        }
        
        await _redisCacheManager.SetValueAsync(0, GetDataDictionaryKey(dto.SessionId, dto.WssdCodeId), 
            JsonConvert.SerializeObject(dataDictionaryCacheList));
    }
    
    private async Task<string> ResolveMenuInputValue(WssdInteractionHandlerDto dto, WssdMenuOption previousMenuOption, WssdMenuOption selectedMenuOption)
    {
        
        if(selectedMenuOption == null)
        {
            //rely on the external source to get the current option
            
            // Check if we have external menu metadata cached
            string menuMetaDataCacheKey = $"menuMetaData:{dto.SessionId}{dto.WssdCodeId}";
            var menuMetaDataCache = await _redisCacheManager.GetValueAsync(0, menuMetaDataCacheKey);
            
            if (!string.IsNullOrEmpty(menuMetaDataCache))
            {
                var externalMenuList = JsonConvert.DeserializeObject<List<ExternalMenuMetaData>>(menuMetaDataCache);
            
                // Find the selected item from the previous menu
                var selectedMenuItem = externalMenuList?.FirstOrDefault(x => 
                    x.UniqueId == dto.SelectedOptionId);
                
                if (selectedMenuItem == null)
                {
                    throw new UserFriendlyException(400, "Selected item is invalid"); 
                }
                
                Logger.Info($"Found matching external menu item: {JsonConvert.SerializeObject(selectedMenuItem)}");
                
                // Use the actual ID value from the external source
                return selectedMenuItem.Id;
            }
            
        }

        if (selectedMenuOption != null)
        {
            //retrieve the menu-input value from the selectedMenuOption
            if (!string.IsNullOrEmpty(selectedMenuOption.InputDefaultValue))
            {
                return selectedMenuOption.InputDefaultValue;
            }
            
        }
        
        // Return the original value if no special handling needed
        return dto.DataDictionary.Value.ToString();
    }
    private async Task<bool> EvaluateFlowControl(WssdInteractionHandlerDto dto, WssdMenuOption previousAction)
    {
        // Extract placeholders from flow control expression
        string stringReplacementPattern = @"\[(.*?)\]";
        var matches = Regex.Matches(previousAction.FlowControlExpression, stringReplacementPattern);
        var placeholders = matches.Cast<Match>().Select(match => match.Groups[1].Value).ToList();
        
        // Retrieve data dictionary
        var dataDictionaryCache = await _redisCacheManager.GetValueAsync(0, GetDataDictionaryKey(dto.SessionId, dto.WssdCodeId));
        if (string.IsNullOrEmpty(dataDictionaryCache))
        {
            throw new UserFriendlyException(400, "Navigation failed from handicap data.");
        }
        
        var dataDictionaryCacheList = JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionaryCache);
        
        // Verify all placeholders exist in data dictionary
        foreach (var placeholder in placeholders)
        {
            var normalizedPlaceholder = StringUtils.NormalizeString(placeholder);
            var dataDictionaryMember = dataDictionaryCacheList.FirstOrDefault(
                x => StringUtils.NormalizeString(x.Key) == normalizedPlaceholder);
            
            if (dataDictionaryMember == null)
            {
                throw new UserFriendlyException(400, "Data dictionary not found.");
            }
        }
        
        Logger.Info($"flow_control_expression triggered => {previousAction.FlowControlExpression}");
        Logger.Info($"obtained data from data_dictionary_cache => {dataDictionaryCache}");
        
        // Convert to dictionary and evaluate
        var dataDictionary = dataDictionaryCacheList.ToDictionary(x => x.Key, x => (object)x.Value);
        var canFlowProceed = FlowControlHandler(previousAction.FlowControlExpression, dataDictionary);
        
        Logger.Info($"flow_control_evaluation_results => {canFlowProceed}");
        return canFlowProceed;
    }
    
    private async Task<object> HandleMenuAction(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {

        var currentMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.Id == dto.CurrentMenuId);

        Logger.Info($"current_menu_in_HandleMenuAction => {JsonConvert.SerializeObject(currentMenu)}");
        
        if (currentMenu == null)
        {
            throw new UserFriendlyException(400, "Menu not found.");
        }

        var currentMenuOption = wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == dto.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        Logger.Info($"current_menu_option_in_HandleMenuAction => {JsonConvert.SerializeObject(currentMenuOption)}");
        
        if (currentMenuOption == null)
        {
            throw new UserFriendlyException(400, "Invalid option.");
        }

        var nextMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.Id == currentMenuOption.NextMenuId);

        Logger.Info($"next_menu_in_HandleMenuAction => {JsonConvert.SerializeObject(nextMenu)}");
        
        if (nextMenu == null)
        {
            throw new UserFriendlyException(400, "Next menu not found.");
        }
 
        var nextMenuOptions = wssdCodeInteractionConfig.MenuOptionList.Where(x => x.MenuId == nextMenu.Id && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)).ToList();

        Logger.Info($"next_menu_options_in_HandleMenuAction => {JsonConvert.SerializeObject(nextMenuOptions)}");
        
        var selectedOptionIndex = nextMenuOptions.FindIndex(x => x.Id == currentMenuOption.Id);
        
        Logger.Info($"selected_option_index_in_HandleMenuAction => {selectedOptionIndex}");
        
        // Check if the next menu has API as data source...
        
        if (nextMenu?.DataSource == "api-response")
        {
            Logger.Info($"nextMenu with datasource from external_api => {JsonConvert.SerializeObject(nextMenu)}");
            
            var ussdCodeInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);

            if (ussdCodeInfo == null)
            {
                throw new UserFriendlyException(400, "Ussd Code not found.");
            }

            var dataDictionaryCache =
                await _redisCacheManager.GetValueAsync(0, $"input:{dto.SessionId}{dto.WssdCodeId}:data");

            List<DataDictionary> dataDictionaryCacheList = string.IsNullOrEmpty(dataDictionaryCache)
                ? new List<DataDictionary>()
                : JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionaryCache);

            // Handle API-based menu options:  call pos api
            var menuRequestPayload = new
            {
                AccountSecret = _hashids.Encode(ussdCodeInfo.TenantId),
                Id = nextMenu.FlexConnectId,
                dto.SessionId,
                dto.WssdCodeId,
                nextMenu.ApiResponsePath,
                nextMenu.ApiResponseKeyField,
                nextMenu.ApiResponseValueTemplate,
                PlaceholderDictionary = dataDictionaryCacheList,
            };
            
            var apiMenuResponse = await _posApi.GenerateMenuFromExternalInterface(menuRequestPayload);
            
            Logger.Info($"apiMenuResponse => {JsonConvert.SerializeObject(apiMenuResponse)}");

            var apiMenuOptionTemplate = nextMenuOptions.Any() ? nextMenuOptions.FirstOrDefault() : null;

            if (apiMenuOptionTemplate == null)
            {
                throw new UserFriendlyException(400, "Menu option template not found.");
            }
            
            // Transform external menu metadata into menu options
            var transformedMenuOptions = apiMenuResponse.Result.ExternalMenuMetaData?.Select((item, index) => new WssdMenuOption 
            {
                Id = item.UniqueId,
                MenuId = nextMenu.Id,
                DisplayText = item.DisplayText,
                ActionType = "menu-input",
                DisplayType = "text",
                InputType = "short-text",
                InputFieldName = apiMenuOptionTemplate.InputFieldName,
                InputDefaultValue = item.Id,
                NextMenuId = apiMenuOptionTemplate.NextMenuId,
                IsVisibleOnDemand = false,
                ExecutionScope = apiMenuOptionTemplate.ExecutionScope,
                
            }).ToList() ?? new List<WssdMenuOption>();
            
            var numberedOptionsFromExternalSource = string.Join("\n", transformedMenuOptions.Select((option, index) => $"{index + 1}. {option.DisplayText}"));
            
            var wssdServiceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
            
            _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
            {
                SessionId = dto.SessionId,
                WssdResourceId = dto.WssdCodeId.ToString(),
                WssdResourceShortName = wssdServiceInfo.WssdShortName,
                ServiceDisplayTitle = wssdServiceInfo.DisplayTitle,
                ActionType = "Menu", 
                IsSuccessful = true, 
                StartTime = DateTime.UtcNow, 
                MenuId = currentMenu.Id, 
                MenuTitle = currentMenu.Title, 
                SelectedOptionId = currentMenuOption.Id, 
                SelectedOptionTitle = currentMenuOption.DisplayText, 
                TenantId = wssdServiceInfo.TenantId,
                TenantName = wssdServiceInfo.TenantName,
                InteractionSource = WssdEngineConsts.UssdInteractionSource,
                Operator = dto.Operator,
                EndTime = DateTime.UtcNow, 
                UserInput = currentMenuOption.DisplayText, 
                SystemResponse = $"{nextMenu.Title}\n{numberedOptionsFromExternalSource}"
            
            });
            
            await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
            {
                CurrentMenuId = dto.CurrentMenuId,
                SelectedOptionId = currentMenuOption.Id,
                CurrentActionType = currentMenuOption.ActionType,
                PreviousOptionId = dto.SelectedOptionId,
                IsAwaitingInput = false,
                IsNextSelectionFromExternalDataSource = true, 
                SelectedOptionIndex = selectedOptionIndex,
                PreviousOptionIndex = 1,
                CurrentMenuTitle = currentMenu.Title, 
                SelectedOptionDisplayText = currentMenuOption.DisplayText, 
                
            });
            
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = request.MSISDN,
                USERDATA = request.USERDATA,
                MSG = $"{nextMenu.Title}\n{numberedOptionsFromExternalSource}" ,
                MSGTYPE = true,
            };
             
        }
        
        //handle 1st menu item being an input ...
        var firstMenuOption = nextMenuOptions.Any() ? nextMenuOptions.FirstOrDefault() : null;
        
        
        if (firstMenuOption is { ActionType: "input" })
        {
            Logger.Info($"action_handler_switched_to_input @ HandleMenuAction");
            
            // trigger input action handler ....
            dto.ActionType = firstMenuOption.ActionType;
            dto.CurrentMenuId = firstMenuOption.MenuId;
            dto.SelectedOptionId = firstMenuOption.Id;
            
            return await HandleInputAction(dto, wssdCodeInteractionConfig, request);

        }
        
        if (firstMenuOption is { ActionType: "display" })
        {
            Logger.Info($"action_handler_switched_to_display @ HandleMenuAction");
            
            // trigger input action handler ....
            dto.ActionType = firstMenuOption.ActionType;
            dto.CurrentMenuId = firstMenuOption.MenuId;
            dto.SelectedOptionId = firstMenuOption.Id;
            
            return await HandleDisplayAction(dto, wssdCodeInteractionConfig, request);
        }
        
        //handle 1st menu item being an api call ...
        if (firstMenuOption is { ActionType: "invocation" })
        {
            Logger.Info($"action_handler_switched_to_api_call @ HandleMenuAction");
            
            // trigger invocation action handler ....
            dto.ActionType = firstMenuOption.ActionType;
            dto.CurrentMenuId = firstMenuOption.MenuId;
            dto.SelectedOptionId = firstMenuOption.Id;
            
            
            
            await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
            {
                CurrentMenuId = dto.CurrentMenuId,
                SelectedOptionId = dto.SelectedOptionId,
                PreviousOptionId = dto.PreviousOptionId,
                CurrentActionType = dto.ActionType,
                IsAwaitingInput = false
            });

            return await ActionSwitchHandler(dto, wssdCodeInteractionConfig, request);

        }
        
        var wssdResourceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
        
        // Reset page counter when switching menus
        await _redisCacheManager.SetValueAsync(0, $"ussd-page:{request.SESSIONID}{dto.WssdCodeId}", "1");

        // Determine if this is the first menu in the flow
        var orderedMenus = wssdCodeInteractionConfig.MenuList.OrderBy(a => a.Order).ToList();
        bool isFirstMenu = orderedMenus.FirstOrDefault()?.Id == nextMenu.Id;
        
        
        // Use pagination if there are many options
        if (nextMenuOptions.Count > DefaultPageSize)
        {
            // Store session interaction for analytics
            _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
            {
                SessionId = dto.SessionId,
                WssdResourceId = dto.WssdCodeId.ToString(),
                WssdResourceShortName = wssdResourceInfo?.WssdShortName,
                ServiceDisplayTitle = wssdResourceInfo?.DisplayTitle,
                ActionType = "Menu",
                IsSuccessful = true,
                StartTime = DateTime.UtcNow,
                MenuId = nextMenu.Id,
                MenuTitle = nextMenu.Title,
                SelectedOptionId = dto.SelectedOptionId,
                SelectedOptionTitle = currentMenuOption.DisplayText,
                TenantId = wssdResourceInfo.TenantId,
                TenantName = wssdResourceInfo.TenantName,
                EndTime = DateTime.UtcNow,
                MobileNo = dto.MobileNo,
                Operator = dto.Operator,
                InteractionSource = WssdEngineConsts.UssdInteractionSource,
                UserInput = dto.UserInput,
                // Update system response to match pagination format
                SystemResponse = $"Paginated menu with {nextMenuOptions.Count} options"
            });

            return CreatePaginatedMenuResponse(request, nextMenu, nextMenuOptions, !isFirstMenu);
        }
        
        // Original implementation for when pagination isn't needed
        var backOptions = !isFirstMenu ? new List<string> { "0. Back", "00. Main Menu" } : new List<string> { "00. Main Menu" };
        
        var numberedDisplayText = string.Join("\n", nextMenuOptions.Select((option, index) => $"{index + 1}. {option.DisplayText}").Concat(backOptions));
         
        _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
        {
            SessionId = dto.SessionId,
            WssdResourceId = wssdResourceInfo.Id.ToString(),
            WssdResourceShortName = wssdResourceInfo.WssdShortName,
            ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
            ActionType = "Menu", 
            IsSuccessful = true, 
            StartTime = DateTime.UtcNow, 
            MenuId = currentMenu.Id, 
            MenuTitle = currentMenu.Title, 
            SelectedOptionId = currentMenuOption.Id, 
            SelectedOptionTitle = currentMenuOption.DisplayText, 
            TenantId = wssdResourceInfo.TenantId,
            TenantName = wssdResourceInfo.TenantName,
            InteractionSource = WssdEngineConsts.UssdInteractionSource,
            EndTime = DateTime.UtcNow, 
            UserInput = dto.UserInput, 
            MobileNo =  dto.MobileNo,
            Operator = dto.Operator,
            SystemResponse = $"{nextMenu.Title}\n{numberedDisplayText}"
            
        });
        
        //update the navigation state
        await UpdateNavigationState(request.SESSIONID, wssdResourceInfo.Id, new NavigationState
        {
            CurrentMenuId = nextMenu.Id,
            SelectedOptionId = currentMenuOption.Id,
            PreviousOptionId = dto.SelectedOptionId,
            CurrentActionType = currentMenuOption.ActionType,
            IsNextSelectionFromExternalDataSource = false,
            IsAwaitingInput = false,
        });
        
        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = $"{nextMenu.Title}\n{numberedDisplayText}",
            MSGTYPE = true,
        };
        
         

    }
    
    private async Task<object> HandleSelectableInputAction(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {

        var currentMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.Id == dto.CurrentMenuId);

        if (currentMenu == null)
        {
            throw new UserFriendlyException(400, "Menu not found.");
        }

        var currentMenuOptions = wssdCodeInteractionConfig.MenuOptionList
            .Where(x => x.MenuId == dto.CurrentMenuId && !x.IsVisibleOnDemand && 
                        (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)).ToList();
        
        int selectedOptionIndex = currentMenuOptions.FindIndex(x => x.Id == dto.SelectedOptionId);

        if (selectedOptionIndex == -1)
        {
            string menuMetaDataCacheKey = $"menuMetaData:{dto.SessionId}{dto.WssdCodeId}";
            var menuMetaDataCache = await _redisCacheManager.GetValueAsync(0, menuMetaDataCacheKey);

            List<ExternalMenuMetaData> externalMenuList = string.IsNullOrEmpty(menuMetaDataCache)
                ? new List<ExternalMenuMetaData>()
                : JsonConvert.DeserializeObject<List<ExternalMenuMetaData>>(menuMetaDataCache);

            selectedOptionIndex = externalMenuList.FindIndex(x => x.UniqueId == dto.SelectedOptionId);
        }
        
        var currentMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == dto.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        Logger.Info($"currentMenuOption in HandleSelectableInputAction => {JsonConvert.SerializeObject(currentMenuOption)}");
        
        if (currentMenuOption == null)
        {
            string menuMetaDataCacheKey = $"menuMetaData:{dto.SessionId}{dto.WssdCodeId}";
            var menuMetaDataCache = await _redisCacheManager.GetValueAsync(0, menuMetaDataCacheKey);
            
            List<ExternalMenuMetaData> externalMenuList = string.IsNullOrEmpty(menuMetaDataCache)
                ? new List<ExternalMenuMetaData>()
                : JsonConvert.DeserializeObject<List<ExternalMenuMetaData>>(menuMetaDataCache);
            
            var selectedMenuOption = externalMenuList.FirstOrDefault(x => x.UniqueId == dto.SelectedOptionId);

            Logger.Info($"selected_menu_option_handleMenuInputAction => {JsonConvert.SerializeObject(selectedMenuOption)}");
            Logger.Info($"currentMenuId_handleMenuInputAction => {dto.CurrentMenuId}");
            
            if (selectedMenuOption == null)
            {
                throw new UserFriendlyException(400, "Invalid option.");
            }
            
            // set props in currentMenuOption using selectedMenuOption
            //get the 1st option in the menu options list as the template for dynamic selectable menu option list
            var menuOptionTemplate =
                wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.MenuId == dto.CurrentMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

            if (menuOptionTemplate == null)
            {
                throw new UserFriendlyException(400, "Menu option template not found.");
            }
            
            Logger.Info($"menuOptionTemplate => {JsonConvert.SerializeObject(menuOptionTemplate)}");
            
            
            currentMenuOption = new WssdMenuOption
            {
                Id = menuOptionTemplate.Id,
                MenuId = dto.CurrentMenuId,
                DisplayText = selectedMenuOption.DisplayText,
                ActionType = "menu-input",
                DisplayType = "text",
                InputType = "short-text",
                InputFieldName = menuOptionTemplate.InputFieldName,
                InputDefaultValue = selectedMenuOption.Id,
                NextMenuId = menuOptionTemplate.NextMenuId,
                IsVisibleOnDemand = false,
                ExecutionScope = menuOptionTemplate.ExecutionScope,
            };
            
            Logger.Info($"computed currentMenuOption from menuOptionTemplate @HandleMenuInputAction => {JsonConvert.SerializeObject(currentMenuOption)}");
            
             
        }

        
        var nextMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.Id == currentMenuOption.NextMenuId);
        
        Logger.Info($"nextMenu in HandleSelectableInputAction => {JsonConvert.SerializeObject(nextMenu)}");

        var nextMenuOptions = wssdCodeInteractionConfig.MenuOptionList.Where(x => nextMenu != null && x.MenuId == nextMenu.Id && !x.IsVisibleOnDemand && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope)).ToList();


        // Check if the next menu has API as data source...
        
        if (nextMenu?.DataSource == "api-response")
        {
            Logger.Info($"nextMenu with datasource from external_api => {JsonConvert.SerializeObject(nextMenu)}");
            
            var ussdCodeInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);

            if (ussdCodeInfo == null)
            {
                throw new UserFriendlyException(400, "Ussd Code not found.");
            }

            var dataDictionaryCache =
                await _redisCacheManager.GetValueAsync(0, $"input:{dto.SessionId}{dto.WssdCodeId}:data");

            List<DataDictionary> dataDictionaryCacheList = string.IsNullOrEmpty(dataDictionaryCache)
                ? new List<DataDictionary>()
                : JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionaryCache);

            // Handle API-based menu options:  call pos api
            var menuRequestPayload = new
            {
                AccountSecret = _hashids.Encode(ussdCodeInfo.TenantId),
                Id = nextMenu.FlexConnectId,
                dto.SessionId,
                dto.WssdCodeId,
                nextMenu.ApiResponsePath,
                nextMenu.ApiResponseKeyField,
                nextMenu.ApiResponseValueTemplate,
                PlaceholderDictionary = dataDictionaryCacheList,
            };
            
            var apiMenuResponse = await _posApi.GenerateMenuFromExternalInterface(menuRequestPayload);
            
            Logger.Info($"apiMenuResponse => {JsonConvert.SerializeObject(apiMenuResponse)}");

            var apiMenuOptionTemplate = nextMenuOptions.Any() ? nextMenuOptions.FirstOrDefault() : null;

            if (apiMenuOptionTemplate == null)
            {
                throw new UserFriendlyException(400, "Menu option template not found.");
            }
            
            // Transform external menu metadata into menu options
            var transformedMenuOptions = apiMenuResponse.Result.ExternalMenuMetaData?.Select((item, index) => new WssdMenuOption 
            {
                Id = item.UniqueId,
                MenuId = nextMenu.Id,
                DisplayText = item.DisplayText,
                ActionType = "menu-input",
                DisplayType = "text",
                InputType = "short-text",
                InputFieldName = apiMenuOptionTemplate.InputFieldName,
                InputDefaultValue = item.Id,
                NextMenuId = apiMenuOptionTemplate.NextMenuId,
                IsVisibleOnDemand = false,
                ExecutionScope = apiMenuOptionTemplate.ExecutionScope,
                
            }).ToList() ?? new List<WssdMenuOption>();
            
            var numberedOptionsFromExternalSource = string.Join("\n", transformedMenuOptions.Select((option, index) => $"{index + 1}. {option.DisplayText}"));
            
            var wssdServiceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
            
            _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
            {
                SessionId = dto.SessionId,
                WssdResourceId = dto.WssdCodeId.ToString(),
                WssdResourceShortName = wssdServiceInfo.WssdShortName,
                ServiceDisplayTitle = wssdServiceInfo.DisplayTitle,
                ActionType = "Menu", 
                IsSuccessful = true, 
                StartTime = DateTime.UtcNow, 
                MenuId = currentMenu.Id, 
                MenuTitle = currentMenu.Title, 
                SelectedOptionId = currentMenuOption.Id, 
                SelectedOptionTitle = currentMenuOption.DisplayText, 
                TenantId = wssdServiceInfo.TenantId,
                TenantName = wssdServiceInfo.TenantName,
                InteractionSource = WssdEngineConsts.UssdInteractionSource,
                Operator = dto.Operator,
                EndTime = DateTime.UtcNow, 
                UserInput = currentMenuOption.DisplayText, 
                SystemResponse = nextMenu != null ? $"{nextMenu.Title}\n{numberedOptionsFromExternalSource}" : $"{numberedOptionsFromExternalSource}"
            
            });
            
            await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
            {
                CurrentMenuId = dto.CurrentMenuId,
                SelectedOptionId = currentMenuOption.Id,
                CurrentActionType = currentMenuOption.ActionType,
                PreviousOptionId = dto.SelectedOptionId,
                IsAwaitingInput = false,
                IsNextSelectionFromExternalDataSource = true, 
                CurrentMenuTitle = currentMenu.Title, 
                SelectedOptionDisplayText = currentMenuOption.DisplayText, 
                
            });
            
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = request.MSISDN,
                USERDATA = request.USERDATA,
                MSG = nextMenu == null ? $"{numberedOptionsFromExternalSource}" : $"{nextMenu.Title}\n{numberedOptionsFromExternalSource}" ,
                MSGTYPE = true,
            };
             
        }
        
        
        //handle 1st menu item being an input ...
        var firstMenuOption = nextMenuOptions.Any() ? nextMenuOptions.FirstOrDefault() : null;

        Logger.Info($"firstMenuOption in HandleSelectableInputAction => {JsonConvert.SerializeObject(firstMenuOption)}");

        if (firstMenuOption is { ActionType: "input" })
        {
            Logger.Info($"action_handler_switched_to_input @ HandleMenuAction");
            // trigger input action handler ....
            dto.ActionType = firstMenuOption.ActionType;
            dto.CurrentMenuId = firstMenuOption.MenuId;
            dto.SelectedOptionId = firstMenuOption.Id;
            return await HandleInputAction(dto, wssdCodeInteractionConfig, request);

        }
        
        if (firstMenuOption is { ActionType: "display" })
        {
            Logger.Info($"action_handler_switched_to_input @ HandleMenuAction");
            // trigger input display handler ....
            dto.ActionType = firstMenuOption.ActionType;
            dto.CurrentMenuId = firstMenuOption.MenuId;
            dto.SelectedOptionId = firstMenuOption.Id;
            return await HandleDisplayAction(dto, wssdCodeInteractionConfig, request);
        }
        
        if (firstMenuOption is { ActionType: "invocation" })
        {
            Logger.Info($"action_handler_switched_to_invocation @ HandleMenuAction");
             
            dto.ActionType = firstMenuOption.ActionType;
            dto.CurrentMenuId = firstMenuOption.MenuId;
            dto.SelectedOptionId = firstMenuOption.Id;
            
            //return await HandleApiCallAction(dto, wssdCodeInteractionConfig, request);
            
            await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
            {
                CurrentMenuId = dto.CurrentMenuId,
                SelectedOptionId = dto.SelectedOptionId,
                PreviousOptionId = dto.PreviousOptionId,
                CurrentActionType = dto.ActionType,
                IsAwaitingInput = false
            });

            return await ActionSwitchHandler(dto, wssdCodeInteractionConfig, request);
        }
        
        var wssdResourceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
        
        var numberedDisplayText = string.Join("\n", nextMenuOptions.Select((option, index) => $"{index + 1}. {option.DisplayText}"));
        
        _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
        {
            SessionId = dto.SessionId,
            WssdResourceId = dto.WssdCodeId.ToString(),
            WssdResourceShortName = wssdResourceInfo.WssdShortName,
            ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
            ActionType = "Menu", 
            IsSuccessful = true, 
            StartTime = DateTime.UtcNow, 
            MenuId = currentMenu.Id, 
            MenuTitle = currentMenu.Title, 
            SelectedOptionId = currentMenuOption.Id, 
            SelectedOptionTitle = currentMenuOption.DisplayText, 
            TenantId = wssdResourceInfo.TenantId,
            TenantName = wssdResourceInfo.TenantName,
            InteractionSource = WssdEngineConsts.UssdInteractionSource,
            Operator = dto.Operator,
            EndTime = DateTime.UtcNow, 
            UserInput = currentMenuOption.DisplayText, 
            SystemResponse = nextMenu != null ? $"{nextMenu.Title}\n{numberedDisplayText}" : $"{numberedDisplayText}"
            
        });
        
        Logger.Info($"numberedDisplayText in HandleSelectableInputAction => {numberedDisplayText}");
        
        await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
        {
            CurrentMenuId = dto.CurrentMenuId,
            SelectedOptionId = currentMenuOption.Id,
            CurrentActionType = currentMenuOption.ActionType,
            PreviousOptionId = dto.SelectedOptionId,
            IsAwaitingInput = false,
            IsNextSelectionFromExternalDataSource = false, 
            SelectedOptionIndex = selectedOptionIndex,
            PreviousOptionIndex = 1,
            CurrentMenuTitle = currentMenu.Title, 
            SelectedOptionDisplayText = currentMenuOption.DisplayText, 
                
        });
        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = nextMenu == null ? $"{numberedDisplayText}" : $"{nextMenu.Title}\n{numberedDisplayText}" ,
            MSGTYPE = nextMenu != null,
        };
        
    }
    
    private async Task<object> HandleDisplayAction(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {
        Logger.Info($"nalo_ussd interaction triggered => {dto.SessionId}");
        
        var currentMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.Id == dto.CurrentMenuId);

        if (currentMenu == null)
        {
            throw new UserFriendlyException(400, "Menu not found.");
        }

        var currentMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == dto.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        if (currentMenuOption == null)
        {
            throw new UserFriendlyException(400, "Invalid option.");
        }

        //check for dynamic display type and resolve placeholders

        //retrieve cache data for data dictionary

        var responseText = currentMenuOption.DisplayOutput;

        //retrieve data state
        var dataDictionaryCache =
            await _redisCacheManager.GetValueAsync(0, $"input:{dto.SessionId}{dto.WssdCodeId}:data");

        if (!string.IsNullOrEmpty(dataDictionaryCache))
        {

            var dataDictionaryCacheList = JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionaryCache);

            var matches = PlaceholderPattern.Matches(currentMenuOption.DisplayOutput);
            foreach (Match match in matches)
            {
                string placeholderKey = match.Groups[1].Value;
                var normalizedPlaceholderKey = StringUtils.NormalizeString(placeholderKey);

                // Find the corresponding placeholder in the PlaceholderDictionary
                var placeholder = dataDictionaryCacheList.Find(p =>
                    StringUtils.NormalizeString(p.Key) == normalizedPlaceholderKey);

                // Check if the placeholder value is a number, and format it if necessary
                if (placeholder != null)
                {
                    // Replace the placeholder with the actual value
                    Logger.Info(
                        $"updated_display_output_value => {responseText}, match_value => {match.Value}, placeholder_value => {placeholder.Value}");
                
                    // Handle different types for placeholder.Value
                    if (placeholder.Value is int || placeholder.Value is long || placeholder.Value is double || placeholder.Value is decimal)
                    {
                        // If it's a numeric type
                        string valueString = placeholder.GetValueAsString();
                        
                        // Check if the number is likely a phone number (10 digits, no decimal point)
                        if (valueString.Length == 10 && valueString.All(char.IsDigit))
                        {
                            // Treat it as a phone number, leave it as-is
                            responseText = responseText.Replace(match.Value, valueString);
                        }
                        else
                        {
                            // Format other numbers to avoid scientific notation
                            if (placeholder.Value is double doubleValue)
                            {
                                responseText = responseText.Replace(match.Value, doubleValue.ToString("0.##########"));
                            }
                            else if (placeholder.Value is decimal decimalValue)
                            {
                                responseText = responseText.Replace(match.Value, decimalValue.ToString("0.##########"));
                            }
                            else
                            {
                                // For int/long, just use the string representation
                                responseText = responseText.Replace(match.Value, valueString);
                            }
                        }
                    }
                    else
                    {
                        // For non-numeric types, just use the string representation
                        responseText = responseText.Replace(match.Value, placeholder.GetValueAsString());
                    }
                }


            }

        }

        Logger.Info($"final_response_text => {responseText}");
        
        //check if the there's a next menu and append 0 and 00 navigation commands to the response text
        var nextMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == currentMenuOption.NextMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
        
        // Determine if this is the last screen in the flow
        bool isLastScreen = nextMenuOption == null && currentMenuOption.ActionType == "display";

        // Only append navigation options if it's not the last screen
        if (!isLastScreen)
        {
            responseText = $"{responseText}\n0. Back\n00. Main Menu";
        }
        
        var wssdResourceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
        
        _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
        {
            SessionId = dto.SessionId,
            WssdResourceId = dto.WssdCodeId.ToString(),
            WssdResourceShortName = wssdResourceInfo.WssdShortName,
            ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
            ActionType = "Display", 
            IsSuccessful = true, 
            StartTime = DateTime.UtcNow, 
            MenuId = currentMenu.Id, 
            MenuTitle = currentMenu.Title, 
            SelectedOptionId = currentMenuOption.Id, 
            SelectedOptionTitle = currentMenuOption.DisplayText, 
            TenantId = wssdResourceInfo.TenantId,
            TenantName = wssdResourceInfo.TenantName,
            InteractionSource = WssdEngineConsts.UssdInteractionSource,
            Operator = dto.Operator,
            EndTime = DateTime.UtcNow, 
            UserInput = currentMenuOption.DisplayText, 
            SystemResponse = $"{currentMenu.Title}\n{responseText} ({currentMenuOption.DisplayType})"
            
        });

        //set the navigation state to the cache
        await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
        {
            CurrentMenuId = dto.CurrentMenuId,
            SelectedOptionId = dto.SelectedOptionId,
            CurrentActionType = dto.ActionType,
            PreviousOptionId = dto.PreviousOptionId,
            IsAwaitingInput = false,
            IsNextSelectionFromExternalDataSource = false,
        });
        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = responseText,
            MSGTYPE = !isLastScreen,
        };
        
    }
     
    private async Task<object> HandleInputAction(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig,NaloUssdSessionRequest request, bool hasRetry = false)
    {
        Logger.Info($"Ussd interaction triggered @ HandleInputAction => {dto.SessionId}");
        
        var currentMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.IsActive() && x.Id == dto.CurrentMenuId);

        if (currentMenu == null)
        {
            throw new UserFriendlyException(400, "Menu not found.");
        }

        var currentMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.Id == dto.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        if (currentMenuOption == null)
        {
            throw new UserFriendlyException(400, "Invalid option.");
        }

        var nextMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.Id == currentMenuOption.NextMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        if (nextMenuOption == null)
        {
            throw new UserFriendlyException(400, "Next action not found.");
        }
        
        var retryCount = 0;
        
        Logger.Info($"handle_input_action has retry => {hasRetry}");
        
        if (hasRetry)
        {
            Logger.Info($"getting retry count in handle_input_action => {dto.SessionId}, {dto.SelectedOptionId}");
            retryCount = await GetRetryCount(dto.SessionId, dto.SelectedOptionId);
        }
        
        Logger.Info($"retry_count in handle_input_action => {retryCount}");
        
        var wssdResourceInfo = await _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
        
        //if input field has default value return a display with 1 to confirm and 0 to deny and terminate the session
        
        if (!string.IsNullOrEmpty(currentMenuOption.InputDefaultValue))
        {
            var confirmationOptions = new List<string> { "1. Confirm", "2. Abort" };
            var confirmationDisplayText = string.Join("\n", confirmationOptions);
            
            _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
            {
                SessionId = dto.SessionId,
                WssdResourceId = dto.WssdCodeId.ToString(),
                WssdResourceShortName = wssdResourceInfo.WssdShortName,
                ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
                ActionType = "Input", 
                IsSuccessful = true, 
                StartTime = DateTime.UtcNow, 
                MenuId = currentMenu.Id, 
                MenuTitle = currentMenu.Title, 
                SelectedOptionId = currentMenuOption.Id, 
                SelectedOptionTitle = currentMenuOption.DisplayText, 
                TenantId = wssdResourceInfo.TenantId,
                TenantName = wssdResourceInfo.TenantName,
                InteractionSource = WssdEngineConsts.UssdInteractionSource,
                Operator = dto.Operator,
                EndTime = DateTime.UtcNow, 
                UserInput = currentMenuOption.DisplayText, 
                SystemResponse = $"{currentMenuOption.InputFieldName}: {currentMenuOption.InputDefaultValue}\n\n{confirmationDisplayText}",
            });
            
            // save current navigation state to cache ...
            
            await _redisCacheManager.SetValueAsync(0, $"ussd-navigation-state:{dto.SessionId}{dto.WssdCodeId}", JsonConvert.SerializeObject(new NavigationState
            {
                CurrentMenuId = currentMenu.Id,
                SelectedOptionId = currentMenuOption.Id,
                CurrentActionType = "input",
                PreviousOptionId = dto.PreviousOptionId
            }));
            
            return new NaloUssdSessionResponse
            {
                USERID = "Rhyolite",
                MSISDN = request.MSISDN,
                USERDATA = request.USERDATA,
                MSG = $"{currentMenuOption.InputFieldName}: {currentMenuOption.InputDefaultValue}\n\n{confirmationDisplayText}",
                MSGTYPE = true,
            };
            
        }
        
        
        _wssdSessionStorageActorProvider.Tell(new WssdSessionInteraction
        {
            SessionId = dto.SessionId,
            WssdResourceId = dto.WssdCodeId.ToString(),
            WssdResourceShortName = wssdResourceInfo.WssdShortName,
            ServiceDisplayTitle = wssdResourceInfo.DisplayTitle,
            ActionType = "Input", 
            IsSuccessful = true, 
            StartTime = DateTime.UtcNow, 
            MenuId = currentMenu.Id, 
            MenuTitle = currentMenu.Title, 
            SelectedOptionId = currentMenuOption.Id, 
            SelectedOptionTitle = currentMenuOption.DisplayText, 
            TenantId = wssdResourceInfo.TenantId,
            TenantName = wssdResourceInfo.TenantName,
            InteractionSource = WssdEngineConsts.UssdInteractionSource,
            Operator = dto.Operator,
            EndTime = DateTime.UtcNow, 
            UserInput = currentMenuOption.DisplayText, 
            SystemResponse = !string.IsNullOrEmpty(currentMenuOption.InputDefaultValue) ? $"{currentMenuOption.InputFieldName}: {currentMenuOption.InputDefaultValue}" : $"{dto.DataDictionary.Key}: {dto.DataDictionary.Value}",
            
        });
        
        await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
        {
            CurrentMenuId = dto.CurrentMenuId,
            SelectedOptionId = currentMenuOption.Id,
            CurrentActionType = currentMenuOption.ActionType,
            PreviousOptionId = dto.SelectedOptionId,
            IsAwaitingInput = true,
            IsNextSelectionFromExternalDataSource = false            
        });
        
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = currentMenuOption.DisplayText,
            MSGTYPE = true,
        };
        
    }
     
    private async Task<object> HandleApiCallAction(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {

        var ussdCodeInfoTask = _repositoryWssdResource.FirstOrDefaultAsync(dto.WssdCodeId);
        
        var dataDictionaryTask = _redisCacheManager.GetValueAsync(0, $"input:{dto.SessionId}{dto.WssdCodeId}:data");
    
        await Task.WhenAll(ussdCodeInfoTask, dataDictionaryTask);
    
        var ussdCodeInfo = await ussdCodeInfoTask;
        var dataDictionaryCache = await dataDictionaryTask;
        
        var settings = new JsonSerializerSettings
        {
            ReferenceLoopHandling = ReferenceLoopHandling.Ignore
        };

        var currentMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.Id == dto.CurrentMenuId);

        if (currentMenu == null)
        {
            throw new UserFriendlyException(400, "Menu not found.");
        }

        var currentMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == dto.SelectedOptionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        if (currentMenuOption == null)
        {
            throw new UserFriendlyException(400, "Invalid option.");
        }
        

        if (ussdCodeInfo == null)
        {
            throw new UserFriendlyException(400, "Ussd Code not found.");
        }
         
        if (!string.IsNullOrEmpty(dataDictionaryCache))
        {

            var dataDictionaryCacheList =
                JsonConvert.DeserializeObject<List<DataDictionary>>(dataDictionaryCache, settings);

            //encode tenantId with hashids
            var accountSecret = _hashids.Encode(ussdCodeInfo.TenantId);

            //use flex connect id to make api call
            var flexConnectPayload = new
            {
                AccountSecret = accountSecret,
                currentMenuOption.ApiOutputVariable,
                Id = currentMenuOption.FlexConnectId,
                dto.SessionId,
                dto.WssdCodeId,
                currentMenuOption.TransformationExpression,
                PlaceholderDictionary = dataDictionaryCacheList,
            };

            //=> 
            var result = await _flexConnectInvocationActor.Ask<ExecuteApiStepResult>(new ExecuteApiStepMessage
            {
                SessionId = dto.SessionId,
                StepId = dto.SelectedOptionId,
                Payload = flexConnectPayload
            }, TimeSpan.FromSeconds(60));

            if (!result.IsSuccessful)
            {
                throw new UserFriendlyException(400, result.ErrorMessage);
            }
        }

        // check the next menu and switch context
        var nextMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == currentMenuOption.NextMenuId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));

        if (nextMenuOption == null)
        {
            throw new UserFriendlyException(400, "Next action not found.");
        }

        if (!string.IsNullOrEmpty(currentMenuOption.FlowControlExpression))
        {

            //get data from the data dictionary cache
            var invocationCache =
                await _redisCacheManager.GetValueAsync(0, $"input:{dto.SessionId}{dto.WssdCodeId}:data");
            var dataDictionaryCacheList =
                string.IsNullOrEmpty(invocationCache)
                    ? new List<DataDictionary>()
                    : JsonConvert.DeserializeObject<List<DataDictionary>>(invocationCache, settings);
            
            
            var dataDictionary = dataDictionaryCacheList.ToDictionary(x => x.Key, x => (object)x.Value);
            
            var canProceed = FlowControlHandler(currentMenuOption.FlowControlExpression, dataDictionary);

            if (!canProceed)
            {
                var fallbackAction = wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.Id == currentMenuOption.FlowControlFallbackActionId && (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
                if (fallbackAction == null)
                {
                    throw new UserFriendlyException(400, "Flow control fallback action not found.");
                }
                
                
                var fallbackDto = new WssdInteractionHandlerDto
                {
                    SessionId = dto.SessionId,
                    WssdCodeId = dto.WssdCodeId,
                    CurrentMenuId = fallbackAction.MenuId,
                    SelectedOptionId = fallbackAction.Id,
                    PreviousOptionId = dto.PreviousOptionId,
                    ActionType = fallbackAction.ActionType,
                    UserInput = dto.UserInput,
                    MobileNo = dto.MobileNo,
                    Operator = dto.Operator,
                    IsHandlingFallback = true
                };
                
                return await ActionSwitchHandler(fallbackDto, wssdCodeInteractionConfig, request);
            }
            
        }

        dto.ActionType = nextMenuOption.ActionType;
        dto.SelectedOptionId = nextMenuOption.Id;
        
        //set the navigation state to the cache
        await UpdateNavigationState(dto.SessionId, dto.WssdCodeId, new NavigationState
        {
            CurrentMenuId = dto.CurrentMenuId,
            SelectedOptionId = nextMenuOption.Id,
            CurrentActionType = nextMenuOption.ActionType,
            PreviousOptionId = dto.PreviousOptionId,
            IsAwaitingInput = false,
            IsNextSelectionFromExternalDataSource = false,
        });
        
        return await ActionSwitchHandler(dto, wssdCodeInteractionConfig, request);

    }

    private async Task<object> HandleDeferredRoutineAction(WssdInteractionHandlerDto dto, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {
        Logger.Info($"ussd interaction triggered @ HandleDeferredRoutineAction => {dto.SessionId}");
        
        var currentMenu = wssdCodeInteractionConfig.MenuList.FirstOrDefault(x => x.IsActive() && x.Id == dto.CurrentMenuId);
        
        if (currentMenu == null)
        {
            throw new UserFriendlyException(400, "Menu not found.");
        }
        
        var currentMenuOption =
            wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(x => x.IsActive() && x.Id == dto.SelectedOptionId && 
            (string.IsNullOrEmpty(x.ExecutionScope) || x.ExecutionScope == WssdEngineConsts.UssdExecutionScope || 
             x.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
        
        if (currentMenuOption == null)
        {
            throw new UserFriendlyException(400, "Invalid option.");
        }
        
        // Schedule the deferred routine via the actor system
        _deferredRoutineActorProvider.Tell(new { dto.SessionId, WssdServiceId = dto.WssdCodeId, WssdOption = currentMenuOption });
        
        // Find the next action after the deferred routine
        var nextActionAfterDeferredRoutine = wssdCodeInteractionConfig.MenuOptionList.FirstOrDefault(a => 
            a.IsActive() && a.Id == currentMenuOption.NextMenuId && 
            (string.IsNullOrEmpty(a.ExecutionScope) || a.ExecutionScope == WssdEngineConsts.UssdExecutionScope || 
             a.ExecutionScope == WssdEngineConsts.UssdWssdExecutionScope));
        
        Logger.Info($"nextActionAfterDeferredRoutine => {JsonConvert.SerializeObject(nextActionAfterDeferredRoutine)}");
        
        if (nextActionAfterDeferredRoutine != null)
        {
            // Update DTO to proceed with the next action
            dto.ActionType = nextActionAfterDeferredRoutine.ActionType;
            dto.SelectedOptionId = nextActionAfterDeferredRoutine.Id;
            
            // Continue with the next action
            return await ActionSwitchHandler(dto, wssdCodeInteractionConfig, request);
        }
        
        // For USSD we need to return a response that can be displayed to the user
            
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = "Session ended successfully.",
            MSGTYPE = true,
        };
    }
    
    private async Task<object> FlowControlRetryHandler(WssdInteractionHandlerDto dto, WssdMenuOption previousAction, WssdResourceInteractionConfig wssdCodeInteractionConfig, NaloUssdSessionRequest request)
    {
        dto.SelectedOptionId = dto.PreviousOptionId;
        dto.ActionType = previousAction.ActionType;
        dto.DataDictionary = null;

        Logger.Info($"current_selected_option in flow_control_retry_handler => {dto}");
       
        // Handle the action type
        switch (dto.ActionType)
        {
            case "menu":
                return await HandleMenuAction(dto, wssdCodeInteractionConfig, request);
            case "menu-input":
                return await HandleSelectableInputAction(dto, wssdCodeInteractionConfig, request);
            case "display":
                return await HandleDisplayAction(dto, wssdCodeInteractionConfig, request);
            case "input":
                return await HandleInputAction(dto, wssdCodeInteractionConfig,request, true);
            case "invocation":
                return await HandleApiCallAction(dto, wssdCodeInteractionConfig, request);
            case "terminate":
                return await HandleTerminateAction(dto);
            case "deferred-routine":
                return await HandleDeferredRoutineAction(dto, wssdCodeInteractionConfig, request);
            default:
                throw new UserFriendlyException(400, "Invalid action type");
        }
    }

    private async Task CacheContextVariablesIfPresent(WssdInteractionHandlerDto dto, WssdMenuOption selectedMenuOption)
    {
        // Check if context variables exist and have values
        if (selectedMenuOption?.ContextVariables == null || !selectedMenuOption.ContextVariables.Any())
        {
            return;
        }

        var cacheKey = $"input:{dto.SessionId}{dto.WssdCodeId}:data";
    
        // Retrieve existing data from cache
        var existingDataJson = await _redisCacheManager.GetValueAsync(0, cacheKey);
        var cachedDataList = string.IsNullOrEmpty(existingDataJson)  ? new List<DataDictionary>() : JsonConvert.DeserializeObject<List<DataDictionary>>(existingDataJson) ?? new List<DataDictionary>();
    
        // Add context variables as DataDictionary entries
        foreach (var contextVariable in selectedMenuOption.ContextVariables)
        {
            // Check if variable already exists and update, or add new
            var existingEntry = cachedDataList.FirstOrDefault(x => x.Key == contextVariable.VariableName);
        
            if (existingEntry != null)
            {
                existingEntry.Value = contextVariable.VariableValue;
                existingEntry.DataType = contextVariable.ValueType;
                existingEntry.Source = "context";
            }
            else
            {
                cachedDataList.Add(new DataDictionary
                {
                    Key = contextVariable.VariableName,
                    Value = contextVariable.VariableValue,
                    DataType = contextVariable.ValueType,
                    Source = "context"
                });
            }
        }
    
        // Save back to cache
        var updatedJson = JsonConvert.SerializeObject(cachedDataList);
        await _redisCacheManager.SetValueAsync(0, cacheKey, updatedJson);
        await _redisCacheManager.SetValueAsync(0, $"transformation:{dto.SessionId}{dto.WssdCodeId}:context", updatedJson);

    }
    
    private async Task<object> HandleTerminateAction(WssdInteractionHandlerDto dto)
    {
        
        //destroy the session...
        await _redisCacheManager.RemoveValueAsync(0, $"nav:{dto.SessionId}{dto.WssdCodeId}:state");
        await _redisCacheManager.RemoveValueAsync(0, $"input:{dto.SessionId}{dto.WssdCodeId}:state");
        await _redisCacheManager.RemoveValueAsync(0, $"ussd-navigation-state:{dto.SessionId}{dto.WssdCodeId}");

         
        return new
        {
            actionType = "terminate"
        };
    }
    
    private async Task<object> HandleTimeout(NaloUssdSessionRequest request, WssdResource wssdResourceInfo)
    {
        Logger.Info($"ussd interaction triggered @ HandleTerminateAction => {request.SESSIONID}");
        
        // Remove session data from cache
        await CleanupSession(request.SESSIONID, wssdResourceInfo.Id);
        
        // Log the timeout event
        Logger.Info($"Session timeout for sessionId: {request.SESSIONID}, wssdResourceId: {wssdResourceInfo.Id}");

        // Return a response indicating the session has timed out
        return new NaloUssdSessionResponse
        {
            USERID = "Rhyolite",
            MSISDN = request.MSISDN,
            USERDATA = request.USERDATA,
            MSG = "Your session has timed out. Please try again.",
            MSGTYPE = true,
        };
        
    }
    
    private async Task SetRetryCountToCache(string sessionId, Guid menuOptionId, int retryCount)
    {
        var retryKey = $"retry:{sessionId}:{menuOptionId}";

        // Check if a retry count already exists
        var existingRetryCount = await _redisCacheManager.GetValueAsync(0, retryKey);
        if (string.IsNullOrEmpty(existingRetryCount))
        {
            // If no retry count exists, set the new retry count
            await _redisCacheManager.SetValueAsync(0, retryKey, retryCount.ToString());
        }
 
    }
    
    private async Task DecrementRetryCount(string sessionId, Guid menuOptionId)
    {
        var retryKey = $"retry:{sessionId}:{menuOptionId}";
        var retryCountCache =  await _redisCacheManager.GetValueAsync(0, retryKey);
        var retryCount = int.Parse(retryCountCache);
        retryCount -= 1;

        if (retryCount == 0)
        {
            await ResetRetryCount(sessionId, menuOptionId);
        }
        else
        {
            await _redisCacheManager.SetValueAsync(0, retryKey, retryCount.ToString());
        }
        
        Logger.Info($"remaining_retries => {retryCount}");
    }

    private async Task<int> GetRetryCount(string sessionId, Guid menuOptionId)
    {
        Logger.Info($"get_retry_count_params sessionId => {sessionId}, menuOptionId => {menuOptionId}");
        var retryKey = $"retry:{sessionId}:{menuOptionId}";
        var retryCountCache =  await _redisCacheManager.GetValueAsync(0, retryKey);
        
        if (!string.IsNullOrEmpty(retryCountCache))
        {
            return int.Parse(retryCountCache);
        }
        return 0;
    }

    private async Task ResetRetryCount(string sessionId, Guid menuOptionId)
    {
        var retryKey = $"retry:{sessionId}:{menuOptionId}";
        await _redisCacheManager.RemoveValueAsync(0, retryKey);
        
        Logger.Info($"retry_count_reset => {retryKey}");
    }
        
    private bool FlowControlHandler(string flowControlExpression, Dictionary<string, object> context)
    {
        try
        {
            
            // For simple validator expressions without logical operators or parentheses
            if (flowControlExpression.Contains("is") && 
                flowControlExpression.Contains("[") && 
                !flowControlExpression.Contains(" AND ") && 
                !flowControlExpression.Contains(" OR ") &&
                !flowControlExpression.Contains("(") && 
                !flowControlExpression.Contains(")"))
            {
                Logger.Info($"using_simple_validator_expression => {flowControlExpression}");
                
                return FlowControlValidator.Validate(flowControlExpression, context);
            }

            Logger.Info($"using_ast_parser for complex expressions => {flowControlExpression}");
            
            // Otherwise, use the AST parser for complex expressions
            var lexer = new Lexer();
            var parser = new Parser(lexer);
            var ast = parser.Parse(flowControlExpression);
            var result = ast.Evaluate(context);
        
            // Ensure the result is a boolean
            if (result is bool boolResult)
                return boolResult;
        
            // If it's not a boolean, attempt conversion
            return Convert.ToBoolean(result);
        }
        catch (Exception ex)
        {
            Logger.Error($"Error evaluating flow control expression: {flowControlExpression}", ex);
            return false;
        }
    }
}