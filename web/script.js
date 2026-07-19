(function () {
  var sidebarLinks = document.querySelectorAll('.sidebar-nav a');
  var sections = [];

  sidebarLinks.forEach(function (link) {
    var href = link.getAttribute('href');
    if (href && href.startsWith('#')) {
      var section = document.getElementById(href.slice(1));
      if (section) sections.push({ el: section, link: link });
    }
  });

  function updateActive() {
    var scrollY = window.scrollY + 80;
    var current = null;

    for (var i = 0; i < sections.length; i++) {
      var s = sections[i];
      if (s.el.offsetTop <= scrollY) {
        current = s;
      }
    }

    sidebarLinks.forEach(function (l) { l.classList.remove('active'); });
    if (current) current.link.classList.add('active');
  }

  window.addEventListener('scroll', updateActive);
  updateActive();
})();
