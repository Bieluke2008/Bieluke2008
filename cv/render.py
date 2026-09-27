from playwright.sync_api import sync_playwright
import pypdfium2 as p, pathlib
src = pathlib.Path('cv.html').resolve()
with sync_playwright() as pw:
    b = pw.chromium.launch(executable_path='/opt/pw-browsers/chromium-1194/chrome-linux/chrome')
    pg = b.new_page()
    pg.goto(src.as_uri()); pg.wait_for_load_state('networkidle')
    pg.evaluate("document.fonts.ready")
    pg.pdf(path='CV_Biel_Vazquez_Cardenas.pdf', format='A4', print_background=True, prefer_css_page_size=True, tagged=True)
    b.close()
d = p.PdfDocument('CV_Biel_Vazquez_Cardenas.pdf'); print('pages', len(d))
for i in range(len(d)): d[i].render(scale=2).to_pil().save(f'new_{i+1}.png')

# Clean document metadata (Chromium leaves the author empty and a browser user agent as creator)
from pypdf import PdfReader, PdfWriter
w = PdfWriter(clone_from='CV_Biel_Vazquez_Cardenas.pdf')
w.add_metadata({'/Title': 'CV – Biel Vázquez Cárdenas', '/Author': 'Biel Vázquez Cárdenas',
                '/Subject': 'Currículum vitae', '/Creator': 'Biel Vázquez Cárdenas'})
w.write('CV_Biel_Vazquez_Cardenas.pdf')
