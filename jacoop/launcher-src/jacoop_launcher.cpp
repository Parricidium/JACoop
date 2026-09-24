// JACoop.exe - native launcher (replaces launcher\jacoop.ps1).
//
//   JACoop.exe                 host: main menu > COOPERATION > CREER (lobby), or solo + F6 > HEBERGER
//   JACoop.exe join [ip[:port]] joiner: straight into that host's lobby, or the COOPERATION > REJOINDRE menu
//   JACoop.exe solo            plain single player with the co-op engine, no networking
//
// Finds Jedi Academy (Steam library folders, GOG registry, usual folders, then
// asks with a folder dialog), remembers it in jacoop.ini next to the exe, and
// starts bin\openjk_sp.x86_64.exe with this folder as its home (config, saves).
// Nothing is ever written into the game's own folder.
//
// Build (x64, VS Build Tools):  build-launcher.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

static std::wstring g_root;		// folder of the exe (the JACoop folder)
static std::wstring g_ini;

// French Windows -> French, anything else -> English (like the game's first language)
static const wchar_t *Tr( const wchar_t *fr, const wchar_t *en ) {
	return ( PRIMARYLANGID( GetUserDefaultUILanguage() ) == LANG_FRENCH ) ? fr : en;
}

static bool FileExists( const std::wstring &p ) {
	DWORD a = GetFileAttributesW( p.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
}
static bool DirExists( const std::wstring &p ) {
	DWORD a = GetFileAttributesW( p.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
}
static std::wstring Join( const std::wstring &a, const std::wstring &b ) {
	if ( a.empty() ) return b;
	if ( a.back() == L'\\' || a.back() == L'/' ) return a + b;
	return a + L"\\" + b;
}
static bool IsGameData( const std::wstring &p ) {
	return !p.empty() && FileExists( Join( p, L"base\\assets0.pk3" ) ) && FileExists( Join( p, L"base\\assets3.pk3" ) );
}

// ---- jacoop.ini (key=value) ----
static std::wstring IniGet( const wchar_t *key ) {
	wchar_t buf[MAX_PATH * 2] = { 0 };
	GetPrivateProfileStringW( L"JACoop", key, L"", buf, (DWORD)( sizeof( buf ) / sizeof( buf[0] ) ), g_ini.c_str() );
	if ( !buf[0] ) {	// older launcher wrote bare key=value lines (no section)
		FILE *f = _wfopen( g_ini.c_str(), L"rt, ccs=UTF-8" );
		if ( f ) {
			wchar_t line[MAX_PATH * 2];
			size_t klen = wcslen( key );
			while ( fgetws( line, (int)( sizeof( line ) / sizeof( line[0] ) ), f ) ) {
				if ( !_wcsnicmp( line, key, klen ) && line[klen] == L'=' ) {
					wcscpy( buf, line + klen + 1 );
					size_t n = wcslen( buf );
					while ( n && ( buf[n - 1] == L'\n' || buf[n - 1] == L'\r' || buf[n - 1] == L' ' ) ) buf[--n] = 0;
					break;
				}
			}
			fclose( f );
		}
	}
	return buf;
}
static void IniSet( const wchar_t *key, const std::wstring &value ) {
	WritePrivateProfileStringW( L"JACoop", key, value.c_str(), g_ini.c_str() );
}

// ---- registry helpers ----
static std::wstring RegString( HKEY root, const wchar_t *subkey, const wchar_t *value, REGSAM extra = 0 ) {
	HKEY h;
	std::wstring out;
	if ( RegOpenKeyExW( root, subkey, 0, KEY_READ | extra, &h ) == ERROR_SUCCESS ) {
		wchar_t buf[MAX_PATH * 2];
		DWORD size = sizeof( buf ), type = 0;
		if ( RegQueryValueExW( h, value, NULL, &type, (LPBYTE)buf, &size ) == ERROR_SUCCESS && ( type == REG_SZ || type == REG_EXPAND_SZ ) ) {
			buf[size / sizeof( wchar_t )] = 0;
			out = buf;
		}
		RegCloseKey( h );
	}
	return out;
}

static void AddSteamLibraries( const std::wstring &steam, std::vector<std::wstring> &cand ) {
	if ( steam.empty() ) return;
	cand.push_back( Join( steam, L"steamapps\\common\\Jedi Academy\\GameData" ) );
	FILE *f = _wfopen( Join( steam, L"steamapps\\libraryfolders.vdf" ).c_str(), L"rb" );
	if ( !f ) return;
	std::string text;
	char buf[4096];
	size_t n;
	while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) text.append( buf, n );
	fclose( f );
	// "path"   "D:\\SteamLibrary"
	size_t pos = 0;
	while ( ( pos = text.find( "\"path\"", pos ) ) != std::string::npos ) {
		size_t q1 = text.find( '"', pos + 6 );
		if ( q1 == std::string::npos ) break;
		size_t q2 = text.find( '"', q1 + 1 );
		if ( q2 == std::string::npos ) break;
		std::string p = text.substr( q1 + 1, q2 - q1 - 1 );
		std::string u;
		for ( size_t i = 0; i < p.size(); i++ ) {	// unescape \\ -> \ .
			if ( p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\' ) { u += '\\'; i++; }
			else u += p[i];
		}
		int wn = MultiByteToWideChar( CP_UTF8, 0, u.c_str(), -1, NULL, 0 );
		std::wstring w( wn ? wn - 1 : 0, 0 );
		if ( wn ) MultiByteToWideChar( CP_UTF8, 0, u.c_str(), -1, &w[0], wn );
		cand.push_back( Join( w, L"steamapps\\common\\Jedi Academy\\GameData" ) );
		pos = q2;
	}
}

static void AddGogGames( HKEY root, const wchar_t *subkey, REGSAM extra, std::vector<std::wstring> &cand ) {
	HKEY h;
	if ( RegOpenKeyExW( root, subkey, 0, KEY_READ | extra, &h ) != ERROR_SUCCESS ) return;
	wchar_t name[256];
	for ( DWORD i = 0;; i++ ) {
		DWORD len = 256;
		if ( RegEnumKeyExW( h, i, name, &len, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) break;
		std::wstring sub = std::wstring( subkey ) + L"\\" + name;
		std::wstring gameName = RegString( root, sub.c_str(), L"gameName", extra );
		std::wstring path = RegString( root, sub.c_str(), L"path", extra );
		if ( !path.empty() && ( gameName.find( L"Jedi Academy" ) != std::wstring::npos || gameName.find( L"JEDI ACADEMY" ) != std::wstring::npos || gameName.find( L"Jedi Knight" ) != std::wstring::npos ) ) {
			cand.push_back( Join( path, L"GameData" ) );
			cand.push_back( path );
		}
	}
	RegCloseKey( h );
}

static std::wstring FindGameData() {
	std::wstring cfg = IniGet( L"GameData" );
	if ( IsGameData( cfg ) ) return cfg;

	std::vector<std::wstring> cand;
	AddSteamLibraries( RegString( HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath" ), cand );
	AddSteamLibraries( RegString( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath" ), cand );
	AddSteamLibraries( RegString( HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath" ), cand );
	AddGogGames( HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, cand );
	AddGogGames( HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, cand );

	const wchar_t *dirs[] = { L"%ProgramFiles%", L"%ProgramFiles(x86)%", L"C:\\GOG Games", L"D:\\GOG Games", L"C:\\Games", L"D:\\Games", L"E:\\Games", L"C:\\SteamLibrary", L"D:\\SteamLibrary", L"E:\\SteamLibrary", L"F:\\SteamLibrary" };
	const wchar_t *subs[] = { L"GOG Galaxy\\Games\\STAR WARS Jedi Knight - Jedi Academy\\GameData", L"Star Wars Jedi Knight - Jedi Academy\\GameData", L"STAR WARS Jedi Knight - Jedi Academy\\GameData",
		L"Jedi Academy\\GameData", L"steamapps\\common\\Jedi Academy\\GameData", L"LucasArts\\Star Wars Jedi Knight Jedi Academy\\GameData" };
	for ( const wchar_t *d : dirs ) {
		wchar_t exp[MAX_PATH];
		ExpandEnvironmentStringsW( d, exp, MAX_PATH );
		if ( exp[0] == L'%' ) continue;	// unexpanded
		for ( const wchar_t *s : subs ) cand.push_back( Join( exp, s ) );
	}
	for ( const std::wstring &c : cand ) {
		if ( IsGameData( c ) ) { IniSet( L"GameData", c ); return c; }
	}

	// ask
	MessageBoxW( NULL, Tr( L"Jedi Academy introuvable (Steam / GOG).\nIndique le dossier GameData du jeu (celui qui contient base\\assets0.pk3).",
		L"Jedi Academy not found (Steam / GOG).\nPoint to the game's GameData folder (the one holding base\\assets0.pk3)." ), L"JACoop", MB_OK | MB_ICONINFORMATION );
	CoInitialize( NULL );
	BROWSEINFOW bi = { 0 };
	bi.lpszTitle = Tr( L"Dossier GameData de Jedi Academy", L"Jedi Academy GameData folder" );
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
	LPITEMIDLIST pidl = SHBrowseForFolderW( &bi );
	if ( pidl ) {
		wchar_t path[MAX_PATH];
		if ( SHGetPathFromIDListW( pidl, path ) ) {
			std::wstring p = path;
			if ( IsGameData( p ) ) { IniSet( L"GameData", p ); CoTaskMemFree( pidl ); return p; }
			if ( IsGameData( Join( p, L"GameData" ) ) ) { p = Join( p, L"GameData" ); IniSet( L"GameData", p ); CoTaskMemFree( pidl ); return p; }
		}
		CoTaskMemFree( pidl );
	}
	MessageBoxW( NULL, Tr( L"Dossier GameData invalide (base\\assets0.pk3 et base\\assets3.pk3 attendus). Relance JACoop.exe.",
		L"Invalid GameData folder (base\\assets0.pk3 and base\\assets3.pk3 expected). Start JACoop.exe again." ), L"JACoop", MB_OK | MB_ICONERROR );
	return L"";
}

// an older version kept the saves under base\saves; they live at the root now
static void MoveOldSaves() {
	std::wstring oldDir = Join( g_root, L"base\\saves" ), newDir = Join( g_root, L"saves" );
	if ( !DirExists( oldDir ) ) return;
	CreateDirectoryW( newDir.c_str(), NULL );
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW( Join( oldDir, L"*" ).c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE ) return;
	do {
		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;
		std::wstring dst = Join( newDir, fd.cFileName );
		if ( !FileExists( dst ) ) MoveFileW( Join( oldDir, fd.cFileName ).c_str(), dst.c_str() );
	} while ( FindNextFileW( h, &fd ) );
	FindClose( h );
	RemoveDirectoryW( oldDir.c_str() );	// only succeeds once empty
}

int WINAPI wWinMain( HINSTANCE, HINSTANCE, LPWSTR, int ) {
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW( NULL, exePath, MAX_PATH );
	g_root = exePath;
	g_root = g_root.substr( 0, g_root.find_last_of( L"\\/" ) );
	g_ini = Join( g_root, L"jacoop.ini" );

	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW( GetCommandLineW(), &argc );
	std::wstring mode = argc > 1 ? argv[1] : L"host";
	std::wstring address = argc > 2 ? argv[2] : L"";
	for ( auto &c : mode ) c = (wchar_t)towlower( c );

	std::wstring gameData = FindGameData();
	if ( gameData.empty() ) return 1;
	MoveOldSaves();

	std::wstring exe = Join( g_root, L"bin\\openjk_sp.x86_64.exe" );
	if ( !FileExists( exe ) ) {
		MessageBoxW( NULL, ( std::wstring( Tr( L"Moteur introuvable :\n", L"Engine not found:\n" ) ) + exe ).c_str(), L"JACoop", MB_OK | MB_ICONERROR );
		return 1;
	}

	std::wstring cmd = L"\"" + exe + L"\" +set fs_basepath \"" + gameData + L"\" +set fs_homepath \"" + g_root + L"\" +exec jacoop.cfg";
	if ( mode == L"solo" ) {
		cmd += L" +set net_enabled 0 +set ui_coopJoin \"\"";
	} else if ( mode == L"join" ) {
		cmd += L" +set net_enabled 1 +set cl_timeout 120";
		if ( !address.empty() ) {
			IniSet( L"LastAddress", address );
			cmd += L" +set ui_coopJoin " + address;
		}
	} else {
		cmd += L" +set net_enabled 1 +set net_port 29070 +set sv_maxclients 4 +set ui_coopJoin \"\"";
	}

	STARTUPINFOW si = { sizeof( si ) };
	PROCESS_INFORMATION pi = { 0 };
	std::wstring cwd = Join( g_root, L"bin" );
	std::vector<wchar_t> cmdBuf( cmd.begin(), cmd.end() );
	cmdBuf.push_back( 0 );
	if ( !CreateProcessW( exe.c_str(), cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, cwd.c_str(), &si, &pi ) ) {
		wchar_t msg[512];
		swprintf( msg, 512, Tr( L"Impossible de lancer le jeu (erreur %lu) :\n%s", L"Could not start the game (error %lu):\n%s" ), GetLastError(), exe.c_str() );
		MessageBoxW( NULL, msg, L"JACoop", MB_OK | MB_ICONERROR );
		return 1;
	}
	CloseHandle( pi.hThread );
	CloseHandle( pi.hProcess );
	return 0;
}
