const fs = require('fs');
const path = require('path');
const QRCode = require('qrcode');

async function make() {
  const upiUrl = 'upi://pay?cu=INR&mc=5817&mode=19&pa=ashaenterprises673694.rzp@rxairtel&tn=Payment%20To%20ASHA%20ENTERPRISES&tr=U439DloJ66t1bZqrv2';
  const passbookUrl = 'https://asha-ro-water.onrender.com/pay/6d0324fb-094c-463a-80ba-87753ee08adc';
  
  const frontQrData = await QRCode.toDataURL(upiUrl, {
    width: 400,
    margin: 2,
    color: { dark: '#15803d', light: '#FFFFFF' }
  });

  const backQrData = await QRCode.toDataURL(passbookUrl, {
    width: 400,
    margin: 2,
    color: { dark: '#0284c7', light: '#FFFFFF' }
  });

  const html = `<!DOCTYPE html>
<html lang="hi">
<head>
  <meta charset="UTF-8">
  <title>RFID Smart Card 2-Sided Sticker - AJAY KUSHWAHA</title>
  <style>
    @page { size: A4 portrait; margin: 10mm; }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }
    body { background: #f1f5f9; padding: 25px; display: flex; flex-direction: column; align-items: center; }
    .print-btn { background: #0284c7; color: #fff; padding: 14px 28px; border-radius: 10px; margin-bottom: 25px; font-weight: 800; cursor: pointer; border: none; font-size: 16px; box-shadow: 0 4px 14px rgba(2,132,199,0.35); }
    .print-btn:hover { background: #0369a1; }
    .instructions { max-width: 650px; background: #fff; border-left: 4px solid #16a34a; padding: 14px 18px; border-radius: 8px; margin-bottom: 20px; font-size: 13.5px; line-height: 1.5; color: #1e293b; box-shadow: 0 2px 8px rgba(0,0,0,0.06); }
    .sheet { background: #fff; padding: 25px; border-radius: 16px; box-shadow: 0 4px 25px rgba(0,0,0,0.1); display: flex; gap: 24px; flex-wrap: wrap; justify-content: center; }
    
    /* Standard ATM/Credit Card Size: 85mm x 54mm */
    .card-sticker {
      width: 85mm; height: 54mm;
      border: 1.5mm solid #16a34a;
      border-radius: 5mm;
      padding: 3mm 4mm;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 3mm;
      background: #ffffff;
      position: relative;
    }
    .badge-side {
      position: absolute;
      top: 2mm; right: 3mm;
      background: #16a34a; color: #fff;
      font-size: 7px; font-weight: 900;
      padding: 1.5px 6px; border-radius: 3px;
      letter-spacing: 0.3px;
    }
    .info-col { flex: 1; display: flex; flex-direction: column; justify-content: center; }
    .brand { font-size: 11px; font-weight: 900; color: #16a34a; letter-spacing: 0.3px; text-transform: uppercase; }
    .loc { font-size: 7.5px; font-weight: 700; color: #64748b; margin-bottom: 2px; }
    .c-name { font-size: 12px; font-weight: 900; color: #0f172a; margin-bottom: 2px; }
    .c-uid { font-size: 8.5px; font-family: monospace; font-weight: 800; color: #0369a1; background: #e0f2fe; padding: 1px 4px; border-radius: 3px; display: inline-block; width: fit-content; margin-bottom: 2px; }
    .rate { font-size: 8px; font-weight: 800; color: #15803d; }
    .scan-tip { font-size: 7.5px; color: #0f172a; margin-top: 2px; line-height: 1.25; font-weight: 700; }
    .hl { font-size: 7.5px; font-weight: 800; color: #0284c7; margin-top: 2px; }
    .qr-col { width: 33mm; height: 33mm; display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
    .qr-col img { width: 100%; height: 100%; object-fit: contain; border-radius: 3px; }

    @media print {
      body { background: transparent; padding: 0; }
      .print-btn, .instructions { display: none !important; }
      .sheet { box-shadow: none; padding: 0; }
    }
  </style>
</head>
<body>
  <button class="print-btn" onclick="window.print()">🖨️ Is 2-Sided Card Sticker Ko Print Karein (Ctrl + P)</button>

  <div class="instructions">
    <b>💡 Card Par Kaise Chipkayein:</b><br>
    • <b>FRONT SIDE (Green):</b> PhonePe / Google Pay / Paytm Direct UPI QR! Isse scan karne par koi browser link nahi khulega, seedhe PhonePe/GPay me ₹10, ₹20, ₹50 daal kar pay hoga aur card me balance jud jayega.<br>
    • <b>BACK SIDE (Blue):</b> Live Passbook & Balance Enquiry QR! Mobile camera se scan karke bacha hua balance aur transactions dekh sakte hain.
  </div>

  <div class="sheet">
    <!-- FRONT SIDE: DIRECT UPI PAYMENT (NO LINK) -->
    <div class="card-sticker" style="border-color:#16a34a">
      <div class="badge-side" style="background:#16a34a">FRONT: DIRECT UPI PAY</div>
      <div class="info-col">
        <div class="brand" style="color:#16a34a">🚰 ASHA RO WATER</div>
        <div class="loc">SMART CARD ATM • BANWARIYA</div>
        <div class="c-name">👤 AJAY KUSHWAHA</div>
        <div class="c-uid">UID: 9E 52 FD 03</div>
        <div class="rate">💧 1 रुपये = 1 लीटर RO पानी</div>
        <div class="scan-tip" style="color:#15803d">⚡ <b>PhonePe / GPay se scan karein</b><br>Direct amount daal kar recharge karein!</div>
        <div class="hl">📞 7700828989</div>
      </div>
      <div class="qr-col">
        <img src="${frontQrData}" alt="Direct UPI QR">
      </div>
    </div>

    <!-- BACK SIDE: LIVE PASSBOOK & BALANCE ENQUIRY -->
    <div class="card-sticker" style="border-color:#0284c7">
      <div class="badge-side" style="background:#0284c7">BACK: LIVE PASSBOOK</div>
      <div class="info-col">
        <div class="brand">📊 ASHA RO WATER</div>
        <div class="loc">PASSBOOK & BALANCE ENQUIRY</div>
        <div class="c-name">👤 AJAY KUSHWAHA</div>
        <div class="c-uid">UID: 9E 52 FD 03</div>
        <div class="rate" style="color:#0284c7">📱 Mobile Camera se scan karein</div>
        <div class="scan-tip">Live Balance aur Transaction Passbook dekhne ke liye scan karein.</div>
        <div class="hl">📞 7700828989</div>
      </div>
      <div class="qr-col">
        <img src="${backQrData}" alt="Live Passbook QR">
      </div>
    </div>
  </div>
</body>
</html>`;

  fs.writeFileSync('C:/Users/Dell/Desktop/PRINT_CARD_STICKER_AJAY.html', html, 'utf8');
  console.log('Successfully written C:/Users/Dell/Desktop/PRINT_CARD_STICKER_AJAY.html');
}

make().catch(console.error);
