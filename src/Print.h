/* Copyright 2015 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

bool PrintFile(const WCHAR* fileName, WCHAR* printerName = nullptr, bool displayErrors = true,
               const WCHAR* settings = nullptr);
bool PrintFile(BaseEngine* engine, WCHAR* printerName = nullptr, bool displayErrors = true,
               const WCHAR* settings = nullptr, WindowInfo* win = nullptr);
void OnMenuPrint(WindowInfo* win, bool waitForCompletion = false);
void AbortPrinting(WindowInfo* win);
