#include <windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <winsvc.h>
#include <wtsapi32.h>
#include "sample.h"
#include <string>
#include <fstream>
#include "sqlite3.h"
#include <queue>
#include <unordered_map>
#include <iostream>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Wtsapi32.lib")


#define SVCNAME TEXT("UATService")

SERVICE_STATUS          gSvcStatus;
SERVICE_STATUS_HANDLE   gSvcStatusHandle;
HANDLE                  ghSvcStopEvent = NULL;

VOID SvcInstall(TCHAR* );
DWORD WINAPI SvcCtrlHandler(DWORD, DWORD, LPVOID, LPVOID);
VOID WINAPI SvcMain(DWORD, LPTSTR*);

VOID ReportSvcStatus(DWORD, DWORD, DWORD);
VOID SvcInit(DWORD, LPTSTR*);
VOID SvcReportEvent(LPTSTR);
std::string errorfile_location, logfile_location, db_location;
std::ofstream errorfile, logfile;
std::queue<DWORD> session_id_queue;
std::unordered_map<DWORD, std::string>session_username;


TCHAR ROOT_PATH[MAX_PATH];

void GetRootPath() {
    TCHAR* lastSlash = _tcsrchr(ROOT_PATH, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';// Keep trailing slash
    }
}
//
// entry point for the process when tryna install the service
//
int __cdecl _tmain(int argc, TCHAR* argv[])
{
    // ignore if not install - when will that be? (when servie is being started)
    TCHAR szUnquotedPath[MAX_PATH];
    GetModuleFileName(NULL, szUnquotedPath, MAX_PATH);
    _tcscpy_s(ROOT_PATH, MAX_PATH, szUnquotedPath);
    GetRootPath();

    if (lstrcmpi(argv[1], TEXT("install")) == 0)
    {
        SvcInstall(szUnquotedPath);
        return 0;
    }

    //
    std::wstring root = ROOT_PATH;
    errorfile_location = std::string(root.begin(), root.end()) + "errorfile.txt";
    logfile_location = std::string(root.begin(), root.end()) + "logfile.txt";
    db_location = std::string(root.begin(), root.end()) + "session_logs.db";

    errorfile.open(errorfile_location, std::ios::app);
    logfile.open(logfile_location, std::ios::app);

    
    //make user db
    sqlite3* DB;
    std::string sql = "create table sessions("
        "session_id integer primary key,"
        "username text,"
        "intime timestamp default current_timestamp,"
        "outtime timestamp,"
        "difference integer);";
    int exit = 0;
    exit = sqlite3_open(db_location.c_str(), &DB); //open our db with name sessions
    char* messaggeError;
    exit = sqlite3_exec(DB, sql.c_str(), NULL, 0, &messaggeError); //execute command

    if (exit != SQLITE_OK) { // if return value is not sqlite_ok there was some error in creating the table
        //std::cerr << "Error Create Table 1" << std::endl;
        sqlite3_free(messaggeError);
        sqlite3_close(DB);
    }
    else {
        //std::cout << "Table created Successfully" << std::endl;
    }

    sql = "create table action_logs("
        "session_id integer primary key,"
        "username text, "
        "action text, "
        "intime timestamp default current_timestamp"
        ");";
    exit = sqlite3_exec(DB, sql.c_str(), NULL, 0, &messaggeError); //execute command 2

    if (exit != SQLITE_OK) { // if return value is not sqlite_ok there was some error in creating the table
        //std::cerr << "Error Create Table" << std::endl;
        sqlite3_free(messaggeError);
        sqlite3_close(DB);
    }
    else {
        //std::cout << "Table created Successfully" << std::endl;
    }

    //closes the connection
    sqlite3_close(DB);
    
    // register our service to the dispatch table + its main function
    SERVICE_TABLE_ENTRY DispatchTable[] =
    {
        { (LPWSTR)SVCNAME, (LPSERVICE_MAIN_FUNCTION)SvcMain },
        { NULL, NULL }
    };

    //if there's no connecting thread from SCM to dispatch table - report this event

    if (!StartServiceCtrlDispatcher(DispatchTable))
    {
        SvcReportEvent((LPWSTR)TEXT("StartServiceCtrlDispatcher"));
    }
}

//
//installs a service in the SCM database
//
VOID SvcInstall(TCHAR* szUnquotedPath)
{
    SC_HANDLE schSCManager;
    SC_HANDLE schService;

    //if .exe file not found
    if (!szUnquotedPath)
    {
        printf("Cannot install service (%d)\n", GetLastError());
        return;
    }

    // all spaces are quoted to avoid errors
    TCHAR szPath[MAX_PATH];
    StringCbPrintf(szPath, MAX_PATH, TEXT("\"%s\""), szUnquotedPath);

    //gets scm service db
    schSCManager = OpenSCManager(
        NULL,                    //local computer
        NULL,                    //ServicesActive database 
        SC_MANAGER_ALL_ACCESS);  //full access rights 

    if (NULL == schSCManager) //if unable to open scm db
    {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return;
    }

    //create our service
    schService = CreateService(
        schSCManager,              // add it scm database 
        SVCNAME,                   // name of the service 
        SVCNAME,                   // service name to display 
        SERVICE_ALL_ACCESS,        // desired access 
        SERVICE_WIN32_OWN_PROCESS, // service type - is the only service in its process
        SERVICE_AUTO_START,      // start type - on boot
        SERVICE_ERROR_NORMAL,      // error control type 
        szPath,                    // path to service's binary 
        NULL,                      // no load ordering group 
        NULL,                      // no tag identifier 
        NULL,                      // no dependencies 
        NULL,                      // LocalSystem account 
        NULL);                     // no password 

    if (schService == NULL) // if nothing returned couldnt create the service
    {
        printf("CreateService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return;
    }
    else printf("Service installed successfully\n");

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}

//
//entry point for the service when its started - obtained from dispatch table
//
VOID WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv)
{
    // register the control code handler function for the service

    gSvcStatusHandle = RegisterServiceCtrlHandlerEx(
        SVCNAME,
        SvcCtrlHandler, NULL);

    if (!gSvcStatusHandle) // if error occured while registering handle
    {
        SvcReportEvent((LPWSTR)TEXT("RegisterServiceCtrlHandler"));
        return;
    }

    // These SERVICE_STATUS members remain as set here

    gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gSvcStatus.dwServiceSpecificExitCode = 0;

    // report initial status to the SCM - service starting and it might take up to 3 seconds
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // service init function
    SvcInit(dwArgc, lpszArgv);
}

// 
// the service init function called by service main
//
VOID SvcInit(DWORD dwArgc, LPTSTR* lpszArgv)
{
    // TO_DO: Declare and set any required variables.
    //   Be sure to periodically call ReportSvcStatus() with 
    //   SERVICE_START_PENDING. If initialization fails, call
    //   ReportSvcStatus with SERVICE_STOPPED.

    // Create an event. The control handler function, SvcCtrlHandler,
    // signals this event when it receives the stop control code.

    ghSvcStopEvent = CreateEvent(
        NULL,    // default security attributes
        TRUE,    // manual reset event
        FALSE,   // not signaled
        NULL);   // no name

    if (ghSvcStopEvent == NULL) //if stop event wasnt created
    {
        ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // running status reported to scm when initialization is complete.

    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // TO_DO: Perform work until service stops.

    while (1)
    {
        // Check whether to stop the service.
        WaitForSingleObject(ghSvcStopEvent, INFINITE);
        ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
    }
}

//
// sets the current service status and reports it to the SCM.
//
VOID ReportSvcStatus(DWORD dwCurrentState,
    DWORD dwWin32ExitCode,
    DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;

    // Fill in the SERVICE_STATUS structure
    // heres where we report which control codes we accept based on current state

    gSvcStatus.dwCurrentState = dwCurrentState;
    gSvcStatus.dwWin32ExitCode = dwWin32ExitCode;
    gSvcStatus.dwWaitHint = dwWaitHint;

    if (dwCurrentState == SERVICE_START_PENDING)
        gSvcStatus.dwControlsAccepted = 0;
    else gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP |
        SERVICE_ACCEPT_SESSIONCHANGE | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SHUTDOWN;



    if ((dwCurrentState == SERVICE_RUNNING) ||
        (dwCurrentState == SERVICE_STOPPED))
        gSvcStatus.dwCheckPoint = 0;
    else gSvcStatus.dwCheckPoint = dwCheckPoint++;

    // Report the status of the service to the SCM.
    SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}



//log to sessions db
static int LogToDB(std::wstring user_activity, DWORD sessionID) {
    
    //connect to db
    sqlite3* DB;
    int exit = 0;
    exit = sqlite3_open(db_location.c_str(), &DB);  //open our db with name sessions

    //error msg and sql statement declaration
    char* messaggeError = nullptr;
    std::string sql;
    sqlite3_stmt* stmt;
    int rc;

	//shutdown doesnt return sessionID
    if (sessionID == NULL) {
        sql="update sessions "
            "set outtime = DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes'), "
            "difference = CAST((julianday(DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes')) "
            "- julianday(intime)) * 86400 * 1000 AS INTEGER) "
            "where session_id = ("
            "select session_id FROM sessions "
            "where outtime IS NULL "
            "order by intime desc "
            "limit 1);  ";
        exit = sqlite3_exec(DB, sql.c_str(), NULL, 0, &messaggeError); //execute command

        if (exit != SQLITE_OK) { // if return value is not sqlite_ok there was some error in creating the table
            //std::cerr << "Error Create Table 1" << std::endl;
            sqlite3_free(messaggeError);
            return 1;
        }
        else {
            //std::cout << "Table created Successfully" << std::endl;
        }

        sql = "insert into action_logs (username, action, intime) "
            "values ("
            "(select username from sessions order by intime desc limit 1),"
            "'shutdown', DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes'));";
        exit = sqlite3_exec(DB, sql.c_str(), NULL, 0, &messaggeError); //execute command

        if (exit != SQLITE_OK) { // if return value is not sqlite_ok there was some error in creating the table
            //std::cerr << "Error Create Table 1" << std::endl;
            sqlite3_free(messaggeError);
            sqlite3_close(DB);
            return 1;
        }
        sqlite3_close(DB);
        return 0;
    }

    //get username
    LPTSTR pBuffer = NULL;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, sessionID, WTSUserName, &pBuffer, &bytes)) {
        return 1;
    }

    //convert username to regular string
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, pBuffer, -1, nullptr, 0, nullptr, nullptr);
    std::string user_name(size_needed, 0); //empty string with size_needed
    WideCharToMultiByte(CP_UTF8, 0, pBuffer, -1, &user_name[0], size_needed, nullptr, nullptr);//normal string

    //convert user_activity to regular string
    size_needed = WideCharToMultiByte(CP_UTF8, 0, user_activity.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string action(size_needed, 0); //empty string with size_needed
    WideCharToMultiByte(CP_UTF8, 0, user_activity.c_str(), -1, &action[0], size_needed, nullptr, nullptr);

    if(user_activity == L"logon" || user_activity == L"unlock") {
        session_id_queue.push(sessionID); //push session id to queue
        session_username[sessionID] = user_name; //map session id to username
	}   

    if (session_id_queue.size() > 5) {
        DWORD oldest_session = session_id_queue.front();
		session_id_queue.pop(); //remove oldest session from queue
		session_username.erase(oldest_session); //remove oldest session from map
    }

    if (user_activity == L"logoff") {
        if (session_username.find(sessionID) != session_username.end()) {
            user_name = session_username[sessionID];
        }
        else {
            user_name = "unknown";
        }
    }

    //insert action into logs
    sql = "insert into action_logs (username, action, intime) values (?, ?, DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes'));";
    rc = sqlite3_prepare_v2(DB, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        if (errorfile.is_open()) {
            errorfile << "cannot prepare insert" << sqlite3_errmsg(DB) << std::endl;
            sqlite3_close(DB);
            return 1; //if insert prep failed, return 1
        }
        //log insert prep error to file
    }
    sqlite3_bind_text(stmt, 1, user_name.c_str(), -1, SQLITE_STATIC);//replace first placeholder with user_name
    sqlite3_bind_text(stmt, 2, action.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt); //execute the statement

    sqlite3_finalize(stmt); //clear statement memory

    //as per user activity, insert or update db
    if (user_activity == L"logon" || user_activity == L"unlock") {
        sql = "insert into sessions (username, intime) values "
            "(?, DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes'));";

        rc = sqlite3_prepare_v2(DB, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            if (errorfile.is_open()) {
                errorfile << "cannot prepare insert" << sqlite3_errmsg(DB) << std::endl;
                sqlite3_close(DB);
				return 1; //if insert prep failed, return 1
            }
            //log insert prep error to file
        }
        sqlite3_bind_text(stmt, 1, user_name.c_str(), -1, SQLITE_STATIC);//replace first placeholder with user_name

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            if (errorfile.is_open()) {
                errorfile << "insert failed: " << sqlite3_errmsg(DB) << std::endl;
                sqlite3_close(DB);
                return 1;
            }
            //log insert fail error to file
        }
        sqlite3_finalize(stmt);
    }else { //for any other user activity
        sql = "update sessions "
            "set outtime = DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes'), "
            "difference = CAST((julianday(DATETIME(CURRENT_TIMESTAMP, '+5 hours', '30 minutes')) "
            "- julianday(intime)) * 86400 * 1000 AS INTEGER) "
            "where session_id = ("
            "select session_id FROM sessions "
            "where username = ? AND outtime IS NULL "
            "order by intime desc "
            "limit 1);  ";

        rc = sqlite3_prepare_v2(DB, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            if (errorfile.is_open()) {
                errorfile << "cannot prepare update" << sqlite3_errmsg(DB) << std::endl;
            }
            //log update prep error to file
            sqlite3_close(DB);
            return 1;
        }
        sqlite3_bind_text(stmt, 1, user_name.c_str(), -1, SQLITE_STATIC);//replace first placeholder with user_name	

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            if (errorfile.is_open()) {
                errorfile << "update failed" << sqlite3_errmsg(DB) << std::endl;
            }
            sqlite3_close(DB);
            return 1; //if update failed, return 1
            //log update fail error to file
        }
        sqlite3_finalize(stmt);
    }
    errorfile.close(); //close error file	
    if (pBuffer) WTSFreeMemory(pBuffer); //clear username buffer
    sqlite3_close(DB);
    return 0;
}

//
//called by SCM whenever a control code is sent to the service using the ControlService function.
//
DWORD WINAPI SvcCtrlHandler(DWORD dwCtrl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext){
    // Handle the requested control code. 
    switch (dwCtrl) //add shutdown and lock functionality in here
    {
    case SERVICE_CONTROL_SESSIONCHANGE: {
        PWTSSESSION_NOTIFICATION pSession = (PWTSSESSION_NOTIFICATION)lpEventData;
        switch (dwEventType) {
            case WTS_SESSION_LOCK: {
                logfile << "locked" << std::endl;
                LogToDB(L"lock", pSession->dwSessionId);
                break;
            }
            case WTS_SESSION_UNLOCK: {
                logfile << "unlocked" << std::endl;
                LogToDB(L"unlock", pSession->dwSessionId);
                break;
            }
            case WTS_SESSION_LOGON: {
                logfile << "logged in" << std::endl;
                LogToDB(L"logon", pSession->dwSessionId);
                break;
            }
            case WTS_SESSION_LOGOFF: {
                logfile << "logged out" << std::endl;
                LogToDB(L"logoff", pSession->dwSessionId);
                break;
            }
        }
        break;
    }
    case SERVICE_CONTROL_PRESHUTDOWN: {
        LogToDB(L"shutdown", NULL);
        ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);
        return S_OK;
    }
    case SERVICE_CONTROL_STOP: {
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

        // Signal the service to stop.

        SetEvent(ghSvcStopEvent);
        ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);

        return S_OK;
    }
    case SERVICE_CONTROL_INTERROGATE: {
        break;
    }
    default:
        break;
    }

}

// 
//logs the messages to the event log
//
VOID SvcReportEvent(LPTSTR szFunction)
{
    HANDLE hEventSource;
    LPCTSTR lpszStrings[2];
    TCHAR Buffer[80];

    hEventSource = RegisterEventSource(NULL, SVCNAME);

    if (NULL != hEventSource)
    {
        StringCchPrintf(Buffer, 80, TEXT("%s failed with %d"), szFunction, GetLastError());

        lpszStrings[0] = SVCNAME;
        lpszStrings[1] = Buffer;

        ReportEvent(hEventSource,        // event log handle
            EVENTLOG_ERROR_TYPE, // event type
            0,                   // event category
            SVC_ERROR,           // event identifier
            NULL,                // no security identifier
            2,                   // size of lpszStrings array
            0,                   // no binary data
            lpszStrings,         // array of strings
            NULL);               // no binary data

        DeregisterEventSource(hEventSource);
    }
}