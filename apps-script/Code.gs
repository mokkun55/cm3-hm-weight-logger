const SHEET_NAME = 'measurements';
const SCRIPT_TOKEN = 'CHANGE_ME_LONG_RANDOM_TOKEN';

function doPost(e) {
  try {
    console.log('doPost received', {
      contentLength: e && e.postData ? e.postData.length : null,
      contentType: e && e.postData ? e.postData.type : null,
      hasContents: Boolean(e && e.postData && e.postData.contents),
    });

    if (!e || !e.postData || !e.postData.contents) {
      console.log('missing post body');
      return jsonResponse({ ok: false, error: 'missing_post_body' });
    }

    const body = JSON.parse(e.postData.contents);
    console.log('parsed body', {
      hasToken: Boolean(body.token),
      weightKg: body.weightKg,
      rawBe: body.rawBe,
      stablePayload: body.stablePayload,
      livePayload: body.livePayload,
      device: body.device,
    });

    if (body.token !== SCRIPT_TOKEN) {
      console.log('unauthorized token');
      return jsonResponse({ ok: false, error: 'unauthorized' });
    }

    const sheet = getOrCreateSheet();
    sheet.appendRow([
      new Date(),
      Number(body.weightKg),
      Number(body.rawBe),
      String(body.stablePayload || ''),
      String(body.livePayload || ''),
      String(body.device || 'CM3-HM'),
    ]);

    const response = {
      ok: true,
      sheet: sheet.getName(),
      row: sheet.getLastRow(),
      weightKg: Number(body.weightKg),
    };
    console.log('append ok', response);
    return jsonResponse(response);
  } catch (error) {
    console.error('doPost failed', error);
    return jsonResponse({ ok: false, error: String(error) });
  }
}

function doGet(e) {
  try {
    console.log('doGet received', {
      hasParameter: Boolean(e && e.parameter),
      queryString: e ? e.queryString : null,
    });

    if (!e || !e.parameter) {
      console.log('missing query parameters');
      return jsonResponse({ ok: false, error: 'missing_query_parameters' });
    }

    return appendMeasurement(e.parameter);
  } catch (error) {
    console.error('doGet failed', error);
    return jsonResponse({ ok: false, error: String(error) });
  }
}

function appendMeasurement(body) {
  console.log('appendMeasurement body', {
    hasToken: Boolean(body.token),
    weightKg: body.weightKg,
    rawBe: body.rawBe,
    stablePayload: body.stablePayload,
    livePayload: body.livePayload,
    device: body.device,
  });

  if (body.token !== SCRIPT_TOKEN) {
    console.log('unauthorized token');
    return jsonResponse({ ok: false, error: 'unauthorized' });
  }

  const sheet = getOrCreateSheet();
  sheet.appendRow([
    new Date(),
    Number(body.weightKg),
    Number(body.rawBe),
    String(body.stablePayload || ''),
    String(body.livePayload || ''),
    String(body.device || 'CM3-HM'),
  ]);

  const response = {
    ok: true,
    sheet: sheet.getName(),
    row: sheet.getLastRow(),
    weightKg: Number(body.weightKg),
  };
  console.log('append ok', response);
  return jsonResponse(response);
}

function getOrCreateSheet() {
  const spreadsheet = SpreadsheetApp.getActiveSpreadsheet();
  let sheet = spreadsheet.getSheetByName(SHEET_NAME);
  if (!sheet) {
    console.log('creating sheet', SHEET_NAME);
    sheet = spreadsheet.insertSheet(SHEET_NAME);
  }

  if (sheet.getLastRow() === 0) {
    console.log('writing header row');
    sheet.appendRow([
      'timestamp',
      'weight_kg',
      'raw_be',
      'stable_payload',
      'live_payload',
      'device',
    ]);
  }

  return sheet;
}

function jsonResponse(body) {
  return ContentService.createTextOutput(JSON.stringify(body))
    .setMimeType(ContentService.MimeType.JSON);
}
